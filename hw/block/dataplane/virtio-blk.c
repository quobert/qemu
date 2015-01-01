/*
 * Dedicated thread for virtio-blk I/O processing
 *
 * Copyright 2012 IBM, Corp.
 * Copyright 2012 Red Hat, Inc. and/or its affiliates
 *
 * Authors:
 *   Stefan Hajnoczi <stefanha@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "trace.h"
#include "qemu/iov.h"
#include "qemu/thread.h"
#include "qemu/error-report.h"
#include "hw/virtio/dataplane/vring.h"
#include "sysemu/block-backend.h"
#include "hw/virtio/virtio-blk.h"
#include "virtio-blk.h"
#include "block/aio.h"
#include "hw/virtio/virtio-bus.h"
#include "qom/object_interfaces.h"

typedef struct {
    EventNotifier notifier;
    VirtIOBlockDataPlane *s;
} VirtIOBlockNotifier;

struct VirtIOBlockDataPlane {
    bool started;
    bool starting;
    bool stopping;
    bool disabled;

    VirtIOBlkConf *conf;

    VirtIODevice *vdev;
    Vring *vring;                    /* virtqueue vring */
    EventNotifier **guest_notifier;  /* irq */
    uint64_t   pending_guest_notifier;  /* pending guest notifer for vq */
    QEMUBH *bh;                     /* bh for guest notification */

    /* Note that these EventNotifiers are assigned by value.  This is
     * fine as long as you do not call event_notifier_cleanup on them
     * (because you don't own the file descriptor or handle; you just
     * use it).
     */
    IOThread *iothread;
    IOThread internal_iothread_obj;
    AioContext *ctx;
    VirtIOBlockNotifier *host_notifier; /* doorbell */
    uint64_t   pending_host_notifier;   /* pending host notifer for vq */
    QEMUBH *host_notifier_bh;           /* for handle host notifier */

    /* Operation blocker on BDS */
    Error *blocker;
    void (*saved_complete_request)(struct VirtIOBlockReq *req,
                                   unsigned char status);
};

/* Raise an interrupt to signal guest, if necessary */
static void notify_guest(VirtIOBlockDataPlane *s, int qid)
{
    if (vring_should_notify(s->vdev, &s->vring[qid])) {
        event_notifier_set(s->guest_notifier[qid]);
    }
}

static void notify_guest_bh(void *opaque)
{
    VirtIOBlockDataPlane *s = opaque;
    unsigned int qid;
    uint64_t pending = s->pending_guest_notifier;

    s->pending_guest_notifier = 0;

    while ((qid = ffsl(pending))) {
        qid--;
        notify_guest(s, qid);
        pending &= ~(1 << qid);
    }
}

static void complete_request_vring(VirtIOBlockReq *req, unsigned char status)
{
    VirtIOBlockDataPlane *s = req->dev->dataplane;
    stb_p(&req->in->status, status);

    vring_push(&s->vring[req->qid], &req->elem,
               req->qiov.size + sizeof(*req->in));

    /* Suppress notification to guest by BH and its scheduled
     * flag because requests are completed as a batch after io
     * plug & unplug is introduced, and the BH can still be
     * executed in dataplane aio context even after it is
     * stopped, so needn't worry about notification loss with BH.
     */
    assert(req->qid < 64);
    s->pending_guest_notifier |= (1 << req->qid);
    qemu_bh_schedule(s->bh);
}

static void process_vq_notify(VirtIOBlockDataPlane *s, unsigned short qid)
{
    VirtIOBlock *vblk = VIRTIO_BLK(s->vdev);

    for (;;) {
        MultiReqBuffer mrb = {};
        int ret;

        /* Disable guest->host notifies to avoid unnecessary vmexits */
        vring_disable_notification(s->vdev, &s->vring[qid]);

        for (;;) {
            VirtIOBlockReq *req = virtio_blk_alloc_request(vblk);

            req->qid = qid;
            ret = vring_pop(s->vdev, &s->vring[qid], &req->elem);
            if (ret < 0) {
                virtio_blk_free_request(req);
                break; /* no more requests */
            }

            trace_virtio_blk_data_plane_process_request(s, req->elem.out_num,
                                                        req->elem.in_num,
                                                        req->elem.index);

            virtio_blk_handle_request(req, &mrb);
        }

        virtio_submit_multireq(s->conf->conf.blk, &mrb);

        if (likely(ret == -EAGAIN)) { /* vring emptied */
            /* Re-enable guest->host notifies and stop processing the vring.
             * But if the guest has snuck in more descriptors, keep processing.
             */
            if (vring_enable_notification(s->vdev, &s->vring[qid])) {
                break;
            }
        } else { /* fatal error */
            break;
        }
    }
}

static void process_notify(void *opaque)
{
    VirtIOBlockDataPlane *s = opaque;
    unsigned int qid;
    uint64_t pending = s->pending_host_notifier;

    s->pending_host_notifier = 0;

    blk_io_plug(s->conf->conf.blk);
    while ((qid = ffsl(pending))) {
        qid--;
        process_vq_notify(s, qid);
        pending &= ~(1 << qid);
    }
    blk_io_unplug(s->conf->conf.blk);
}

/* TODO: handle requests from other vqs together */
static void handle_notify(EventNotifier *e)
{
    VirtIOBlockNotifier *n = container_of(e, VirtIOBlockNotifier,
                                         notifier);
    VirtIOBlockDataPlane *s = n->s;
    unsigned int qid = n - &s->host_notifier[0];

    assert(qid < 64);

    event_notifier_test_and_clear(e);

    s->pending_host_notifier |= (1 << qid);
    qemu_bh_schedule(s->host_notifier_bh);
}


/* Context: QEMU global mutex held */
void virtio_blk_data_plane_create(VirtIODevice *vdev, VirtIOBlkConf *conf,
                                  VirtIOBlockDataPlane **dataplane,
                                  Error **errp)
{
    VirtIOBlockDataPlane *s;
    Error *local_err = NULL;
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);

    *dataplane = NULL;

    if (!conf->data_plane && !conf->iothread) {
        return;
    }

    /* Don't try if transport does not support notifiers. */
    if (!k->set_guest_notifiers || !k->set_host_notifier) {
        error_setg(errp,
                   "device is incompatible with x-data-plane "
                   "(transport does not support notifiers)");
        return;
    }

    /* If dataplane is (re-)enabled while the guest is running there could be
     * block jobs that can conflict.
     */
    if (blk_op_is_blocked(conf->conf.blk, BLOCK_OP_TYPE_DATAPLANE,
                          &local_err)) {
        error_setg(errp, "cannot start dataplane thread: %s",
                   error_get_pretty(local_err));
        error_free(local_err);
        return;
    }

    s = g_new0(VirtIOBlockDataPlane, 1);
    s->vdev = vdev;
    s->conf = conf;

    if (conf->iothread) {
        s->iothread = conf->iothread;
        object_ref(OBJECT(s->iothread));
    } else {
        /* Create per-device IOThread if none specified.  This is for
         * x-data-plane option compatibility.  If x-data-plane is removed we
         * can drop this.
         */
        object_initialize(&s->internal_iothread_obj,
                          sizeof(s->internal_iothread_obj),
                          TYPE_IOTHREAD);
        user_creatable_complete(OBJECT(&s->internal_iothread_obj), &error_abort);
        s->iothread = &s->internal_iothread_obj;
    }
    s->ctx = iothread_get_aio_context(s->iothread);
    s->bh = aio_bh_new(s->ctx, notify_guest_bh, s);

    s->vring = g_new0(Vring, s->conf->num_queues);
    s->guest_notifier = g_new(EventNotifier *, s->conf->num_queues);
    s->host_notifier = g_new(VirtIOBlockNotifier, s->conf->num_queues);
    s->host_notifier_bh = aio_bh_new(s->ctx, process_notify, s);

    error_setg(&s->blocker, "block device is in use by data plane");
    blk_op_block_all(conf->conf.blk, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_RESIZE, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_DRIVE_DEL, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_BACKUP_SOURCE, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_CHANGE, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_COMMIT, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_EJECT, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_EXTERNAL_SNAPSHOT, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_INTERNAL_SNAPSHOT, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_INTERNAL_SNAPSHOT_DELETE,
                   s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_MIRROR, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_STREAM, s->blocker);
    blk_op_unblock(conf->conf.blk, BLOCK_OP_TYPE_REPLACE, s->blocker);

    *dataplane = s;
}

/* Context: QEMU global mutex held */
void virtio_blk_data_plane_destroy(VirtIOBlockDataPlane *s)
{
    if (!s) {
        return;
    }

    virtio_blk_data_plane_stop(s);
    blk_op_unblock_all(s->conf->conf.blk, s->blocker);
    error_free(s->blocker);
    object_unref(OBJECT(s->iothread));
    qemu_bh_delete(s->bh);
    qemu_bh_delete(s->host_notifier_bh);
    g_free(s->vring);
    g_free(s->guest_notifier);
    g_free(s->host_notifier);
    g_free(s);
}

static int pre_start_vq(VirtIOBlockDataPlane *s, BusState *qbus,
                        VirtioBusClass *k)
{
    int j, i, ret = -1;
    int num = s->conf->num_queues;
    VirtQueue *vq[num];

    for (i = 0; i < num; i++) {
        vq[i] = virtio_get_queue(s->vdev, i);
        if (!vring_setup(&s->vring[i], s->vdev, i)) {
            goto fail_vring;
        }
    }

    /* Set up guest notifier (irq) */
    if (k->set_guest_notifiers(qbus->parent, num, true) != 0) {
        fprintf(stderr, "virtio-blk failed to set guest notifier, "
                "ensure -enable-kvm is set\n");
       s->disabled = true;
        goto fail_vring;
    }

    for (i = 0; i < num; i++) {
        s->guest_notifier[i] = virtio_queue_get_guest_notifier(vq[i]);
    }
    s->pending_guest_notifier = 0;

    /* Set up virtqueue notify */
    for (i = 0; i < num; i++) {
        if (k->set_host_notifier(qbus->parent, i, true) != 0) {
            fprintf(stderr, "virtio-blk failed to set host notifier\n");
           goto fail_host_notifiers;
        }
        s->host_notifier[i].notifier = *virtio_queue_get_host_notifier(vq[i]);
        s->host_notifier[i].s = s;
    }
    s->pending_host_notifier = 0;

    return 0;

fail_host_notifiers:
    for (j = 0; j < i; j++) {
        k->set_host_notifier(qbus->parent, j, true);
    }
    k->set_guest_notifiers(qbus->parent, num, false);
    i = num;

fail_vring:
    for (j = 0; j < i; j++) {
        vring_teardown(&s->vring[j], s->vdev, j);
    }
    return ret;
}

static void post_start_vq(VirtIOBlockDataPlane *s)
{
    int i;
    int num = s->conf->num_queues;

    for (i = 0; i < num; i++) {
        VirtQueue *vq;
        vq = virtio_get_queue(s->vdev, i);

        /* Kick right away to begin processing requests already in vring */
        event_notifier_set(virtio_queue_get_host_notifier(vq));
    }

    /* Get this show started by hooking up our callbacks */
    aio_context_acquire(s->ctx);
    for (i = 0; i < num; i++)
        aio_set_event_notifier(s->ctx, &s->host_notifier[i].notifier,
                               handle_notify);
    aio_context_release(s->ctx);
}


/* Context: QEMU global mutex held */
void virtio_blk_data_plane_start(VirtIOBlockDataPlane *s)
{
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(s->vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    VirtIOBlock *vblk = VIRTIO_BLK(s->vdev);

    if (s->started || s->disabled) {
        return;
    }

    if (s->starting) {
        return;
    }

    s->starting = true;

    s->saved_complete_request = vblk->complete_request;
    vblk->complete_request = complete_request_vring;

    if (pre_start_vq(s, qbus, k)) {
        s->starting = false;
        return;
    }

    s->starting = false;
    s->started = true;
    trace_virtio_blk_data_plane_start(s);

    blk_set_aio_context(s->conf->conf.blk, s->ctx);

    post_start_vq(s);
}

/* Context: QEMU global mutex held */
void virtio_blk_data_plane_stop(VirtIOBlockDataPlane *s)
{
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(s->vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    VirtIOBlock *vblk = VIRTIO_BLK(s->vdev);
    int i;
    int num = s->conf->num_queues;

    /* Better luck next time. */
    if (s->disabled) {
        s->disabled = false;
        return;
    }
    if (!s->started || s->stopping) {
        return;
    }
    s->stopping = true;
    vblk->complete_request = s->saved_complete_request;
    trace_virtio_blk_data_plane_stop(s);

    aio_context_acquire(s->ctx);

    /* Stop notifications for new requests from guest */
    for (i = 0; i < num; i++) {
        aio_set_event_notifier(s->ctx, &s->host_notifier[i].notifier, NULL);
    }

    /* Drain and switch bs back to the QEMU main loop */
    blk_set_aio_context(s->conf->conf.blk, qemu_get_aio_context());

    aio_context_release(s->ctx);

    /* Sync vring state back to virtqueue so that non-dataplane request
     * processing can continue when we disable the host notifier below.
     */
    for (i = 0; i < num; i++) {
        vring_teardown(&s->vring[i], s->vdev, 0);
    }

    for (i = 0; i < num; i++) {
        k->set_host_notifier(qbus->parent, i, false);
    }

    /* Clean up guest notifier (irq) */
    k->set_guest_notifiers(qbus->parent, num, false);

    s->started = false;
    s->stopping = false;
}
