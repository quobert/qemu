/*
 * Linux native AIO support.
 *
 * Copyright (C) 2009 IBM, Corp.
 * Copyright (C) 2009 Red Hat, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include "qemu-common.h"
#include "block/aio.h"
#include "qemu/queue.h"
#include "block/raw-aio.h"
#include "qemu/event_notifier.h"
#include "block/coroutine.h"

#include <libaio.h>

/*
 * Queue size (per-device).
 *
 * XXX: eventually we need to communicate this to the guest and/or make it
 *      tunable by the guest.  If we get more outstanding requests at a time
 *      than this we will get EAGAIN from io_submit which is communicated to
 *      the guest as an I/O error.
 */
#define MAX_EVENTS 128

#define MAX_QUEUED_IO  128

struct qemu_laiocb {
    Coroutine *co;
    struct qemu_laio_state *ctx;
    struct iocb iocb;
    ssize_t ret;
    size_t nbytes;
    QEMUIOVector *qiov;
    bool is_read;
    QLIST_ENTRY(qemu_laiocb) node;
};

typedef struct {
    struct iocb *iocbs[MAX_QUEUED_IO];
    int plugged;
    unsigned int size;
    unsigned int idx;
} LaioQueue;

struct qemu_laio_state {
    io_context_t ctx;
    EventNotifier e;

    /* io queue for submit at batch */
    LaioQueue io_q;
};

static void laio_submit_co_cb(void *opaque, int ret);

static inline ssize_t io_event_ret(struct io_event *ev)
{
    return (ssize_t)(((uint64_t)ev->res2 << 32) | ev->res);
}

/*
 * Completes an AIO request (calls the callback and frees the ACB).
 */
static void qemu_laio_process_completion(struct qemu_laio_state *s,
    struct qemu_laiocb *laiocb)
{
    int ret;

    ret = laiocb->ret;
    if (ret != -ECANCELED) {
        if (ret == laiocb->nbytes) {
            ret = 0;
        } else if (ret >= 0) {
            /* Short reads mean EOF, pad with zeros. */
            if (laiocb->is_read) {
                qemu_iovec_memset(laiocb->qiov, ret, 0,
                    laiocb->qiov->size - ret);
            } else {
                ret = -EINVAL;
            }
        }

        laio_submit_co_cb(laiocb, ret);
    }
}

static void qemu_laio_completion_cb(EventNotifier *e)
{
    struct qemu_laio_state *s = container_of(e, struct qemu_laio_state, e);

    while (event_notifier_test_and_clear(&s->e)) {
        struct io_event events[MAX_EVENTS];
        struct timespec ts = { 0 };
        int nevents, i;

        do {
            nevents = io_getevents(s->ctx, MAX_EVENTS, MAX_EVENTS, events, &ts);
        } while (nevents == -EINTR);

        for (i = 0; i < nevents; i++) {
            struct iocb *iocb = events[i].obj;
            struct qemu_laiocb *laiocb =
                    container_of(iocb, struct qemu_laiocb, iocb);

            laiocb->ret = io_event_ret(&events[i]);
            qemu_laio_process_completion(s, laiocb);
        }
    }
}

static void ioq_init(LaioQueue *io_q)
{
    io_q->size = MAX_QUEUED_IO;
    io_q->idx = 0;
    io_q->plugged = 0;
}

static int ioq_submit(struct qemu_laio_state *s)
{
    int ret, i = 0;
    int len = s->io_q.idx;

    do {
        ret = io_submit(s->ctx, len, s->io_q.iocbs);
    } while (i++ < 3 && ret == -EAGAIN);

    /* empty io queue */
    s->io_q.idx = 0;

    if (ret < 0) {
        i = 0;
    } else {
        i = ret;
    }

    for (; i < len; i++) {
        struct qemu_laiocb *laiocb =
            container_of(s->io_q.iocbs[i], struct qemu_laiocb, iocb);

        laiocb->ret = (ret < 0) ? ret : -EIO;
        qemu_laio_process_completion(s, laiocb);
    }
    return ret;
}

static void ioq_enqueue(struct qemu_laio_state *s, struct iocb *iocb)
{
    unsigned int idx = s->io_q.idx;

    s->io_q.iocbs[idx++] = iocb;
    s->io_q.idx = idx;

    /* submit immediately if queue is full */
    if (idx == s->io_q.size) {
        ioq_submit(s);
    }
}

void laio_io_plug(BlockDriverState *bs, void *aio_ctx)
{
    struct qemu_laio_state *s = aio_ctx;

    s->io_q.plugged++;
}

int laio_io_unplug(BlockDriverState *bs, void *aio_ctx, bool unplug)
{
    struct qemu_laio_state *s = aio_ctx;
    int ret = 0;

    assert(s->io_q.plugged > 0 || !unplug);

    if (unplug && --s->io_q.plugged > 0) {
        return 0;
    }

    if (s->io_q.idx > 0) {
        ret = ioq_submit(s);
    }

    return ret;
}

static void laio_submit_co_cb(void *opaque, int ret)
{
    struct qemu_laiocb *laiocb = opaque;
    laiocb->ret = ret;
    qemu_coroutine_enter(laiocb->co, NULL);
}

int laio_submit_co(BlockDriverState *bs, void *aio_ctx, int fd,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors, int type)
{
    struct qemu_laio_state *s = aio_ctx;
    off_t offset = sector_num * 512;

    struct qemu_laiocb laiocb = {
        .co = qemu_coroutine_self(),
        .nbytes = nb_sectors * 512,
        .ctx = s,
        .is_read = (type == QEMU_AIO_READ),
        .qiov = qiov,
    };
    struct iocb *iocbs = &laiocb.iocb;

    switch (type) {
    case QEMU_AIO_WRITE:
        io_prep_pwritev(iocbs, fd, qiov->iov, qiov->niov, offset);
	break;
    case QEMU_AIO_READ:
        io_prep_preadv(iocbs, fd, qiov->iov, qiov->niov, offset);
	break;
    /* Currently Linux kernel does not support other operations */
    default:
        fprintf(stderr, "%s: invalid AIO request type 0x%x.\n",
                        __func__, type);
        return -EIO;
    }
    io_set_eventfd(&laiocb.iocb, event_notifier_get_fd(&s->e));

    if (!s->io_q.plugged) {
        if (io_submit(s->ctx, 1, &iocbs) < 0) {
            return -EIO;
        }
    } else {
        ioq_enqueue(s, iocbs);
    }

    qemu_coroutine_yield();
    return laiocb.ret;
}

void laio_detach_aio_context(void *s_, AioContext *old_context)
{
    struct qemu_laio_state *s = s_;

    aio_set_event_notifier(old_context, &s->e, NULL);
}

void laio_attach_aio_context(void *s_, AioContext *new_context)
{
    struct qemu_laio_state *s = s_;

    aio_set_event_notifier(new_context, &s->e, qemu_laio_completion_cb);
}

void *laio_init(void)
{
    struct qemu_laio_state *s;

    s = g_malloc0(sizeof(*s));
    if (event_notifier_init(&s->e, false) < 0) {
        goto out_free_state;
    }

    if (io_setup(MAX_EVENTS, &s->ctx) != 0) {
        goto out_close_efd;
    }

    ioq_init(&s->io_q);

    return s;

out_close_efd:
    event_notifier_cleanup(&s->e);
out_free_state:
    g_free(s);
    return NULL;
}

void laio_cleanup(void *s_)
{
    struct qemu_laio_state *s = s_;

    event_notifier_cleanup(&s->e);

    if (io_destroy(s->ctx) != 0) {
        fprintf(stderr, "%s: destroy AIO context %p failed\n",
                        __func__, &s->ctx);
    }
    g_free(s);
}
