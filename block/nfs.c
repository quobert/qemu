/*
 * QEMU Block driver for native access to files on NFS shares
 *
 * Copyright (c) 2013 Peter Lieven <pl@kamp.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config-host.h"

#include <poll.h>
#include <arpa/inet.h>
#include "qemu-common.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "block/block_int.h"
#include "trace.h"
#include "block/scsi.h"
#include "qemu/iov.h"
#include "sysemu/sysemu.h"
#include "qmp-commands.h"

#include <nfsc/libnfs-zdr.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-mount.h>

typedef struct nfsclient {
    struct nfs_context *context;
    struct nfsfh *fh;
    int events;
} nfsclient;

typedef struct NFSTask {
    int status;
    int complete;
    void *data;
    QEMUIOVector *iov;
    Coroutine *co;
} NFSTask;

static void nfs_process_read(void *arg);
static void nfs_process_write(void *arg);

static void nfs_set_events(nfsclient *client)
{
    int ev;

    /* We always register a read handler.  */
    ev = POLLIN;
    ev |= nfs_which_events(client->context);
    if (ev != client->events) {
        qemu_aio_set_fd_handler(nfs_get_fd(client->context),
                      nfs_process_read,
                      (ev & POLLOUT) ? nfs_process_write : NULL,
                      client);

    }

    client->events = ev;
}

static void nfs_process_read(void *arg)
{
    nfsclient *client = arg;

    nfs_service(client->context, POLLIN);
    nfs_set_events(client);
}

static void nfs_process_write(void *arg)
{
    nfsclient *client = arg;

    nfs_service(client->context, POLLOUT);
    nfs_set_events(client);
}

static void nfs_co_init_task(nfsclient *client, NFSTask *Task)
{
    *Task = (NFSTask) {
        .co         = qemu_coroutine_self(),
    };
}

static void nfs_co_generic_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	NFSTask *Task = private_data;
    fprintf(stderr,"nfs_co_generic_cb\n");
    Task->complete = 1;
    Task->status = status;
    Task->data = data;
    if (Task->status > 0 && Task->iov) {
		qemu_iovec_from_buf(Task->iov, 0, data, status);
	}
    if (Task->co) {
        qemu_coroutine_enter(Task->co, NULL);
    }
}

static int coroutine_fn nfs_co_readv(BlockDriverState *bs,
                                     int64_t sector_num, int nb_sectors,
                                     QEMUIOVector *iov)
{
    nfsclient *client = bs->opaque;
    struct NFSTask Task;
    
    nfs_co_init_task(client, &Task);
    Task.iov = iov;
    fprintf(stderr,"nfs_co_readv %lld %d\n",(long long int) sector_num, nb_sectors);
    if (nfs_pread_async(client->context, client->fh, sector_num * BDRV_SECTOR_SIZE,
                        nb_sectors * BDRV_SECTOR_SIZE, nfs_co_generic_cb, &Task) != 0) {
        return -EIO;
    }

    while (!Task.complete) {
        nfs_set_events(client);
        qemu_coroutine_yield();
    }
    
    if (Task.status != nb_sectors * BDRV_SECTOR_SIZE) {
        return -EIO;
    }
    
    fprintf(stderr,"read %d bytes\n", Task.status);
	return 0;
}

static int coroutine_fn nfs_co_writev(BlockDriverState *bs,
                                        int64_t sector_num, int nb_sectors,
                                        QEMUIOVector *iov)
{
	return -EINVAL;
}

static int coroutine_fn nfs_co_flush(BlockDriverState *bs)
{
	return -EINVAL;
}

/* TODO Convert to fine grained options */
static QemuOptsList runtime_opts = {
    .name = "nfs",
    .head = QTAILQ_HEAD_INITIALIZER(runtime_opts.head),
    .desc = {
        {
            .name = "filename",
            .type = QEMU_OPT_STRING,
            .help = "URL to the NFS file",
        },
        { /* end of list */ }
    },
};

static void nfs_file_close(BlockDriverState *bs)
{
    fprintf(stderr,"nfs_file_close\n");
    nfsclient *client = bs->opaque;
    if (client->fh) {
		nfs_close(client->context, client->fh);
	}
    if (client->context) {
        qemu_aio_set_fd_handler(nfs_get_fd(client->context), NULL, NULL, NULL);
        nfs_destroy_context(client->context);
    }
    memset(client, 0, sizeof(nfsclient));
}

static int nfs_file_open(BlockDriverState *bs, QDict *options, int flags,
                         Error **errp)
{
    nfsclient *client = bs->opaque;
    const char *filename;
    int ret = 0;
    QemuOpts *opts;
    Error *local_err = NULL;
    char *server = NULL, *path = NULL, *file = NULL, *strp;
    struct stat st;

    opts = qemu_opts_create_nofail(&runtime_opts);
    qemu_opts_absorb_qdict(opts, options, &local_err);
    if (error_is_set(&local_err)) {
        qerror_report_err(local_err);
        error_free(local_err);
        ret = -EINVAL;
        goto fail;
    }

    filename = qemu_opt_get(opts, "filename");
	fprintf(stderr,"nfs_file_open\n");
    
    client->context = nfs_init_context();
	
	if (client->context == NULL) {
		fprintf(stderr, "failed to init context\n");
		ret = -EINVAL;
		goto fail;
	}

	server = g_strdup(filename + 6);
	if (server[0] == '/' || server[0] == '\0') {
		fprintf(stderr, "Invalid server string.\n");
		ret = -EINVAL;
		goto fail;
	}
	strp = strchr(server, '/');
	if (strp == NULL) {
		fprintf(stderr, "Invalid URL specified.\n");
		ret = -EINVAL;
		goto fail;
	}
	path = g_strdup(strp);
	*strp = 0;
	strp = strrchr(path, '/');
	if (strp == NULL) {
		fprintf(stderr, "Invalid URL specified.\n");
		ret = -EINVAL;
		goto fail;
	}
	file = g_strdup(strp);
	*strp = 0;

	if (nfs_mount(client->context, server, path) != 0) {
 		fprintf(stderr, "Failed to mount nfs share : %s\n",
			       nfs_get_error(client->context));
		ret = -EINVAL;
		goto fail;
	}

	if (nfs_open(client->context, file, O_RDONLY, &client->fh) != 0) {
		fprintf(stderr, "Failed to open file : %s\n",
				        nfs_get_error(client->context));
		ret = -EINVAL;
		goto fail;
	}

	if (nfs_fstat(client->context, client->fh, &st) != 0) {
		fprintf(stderr, "Failed to fstat source file\n");
		ret = -EIO;
		goto fail;
	}

    bs->total_sectors = st.st_size / BDRV_SECTOR_SIZE;

    //~ struct nfs_context *nfs = NULL;
    //~ const char *filename;
    //~ int ret;
    goto out;
fail:
    nfs_file_close(bs);
out:
    g_free(server);
    g_free(path);
    g_free(file);
    return ret;
}

static BlockDriver bdrv_nfs = {
    .format_name     = "nfs",
    .protocol_name   = "nfs",

    .instance_size   = sizeof(nfsclient),
    .bdrv_needs_filename = true,
    .bdrv_file_open  = nfs_file_open,
    .bdrv_close      = nfs_file_close,

    .bdrv_co_readv         = nfs_co_readv,
    .bdrv_co_writev        = nfs_co_writev,
    .bdrv_co_flush_to_disk = nfs_co_flush,
};

static void nfs_block_init(void)
{
    bdrv_register(&bdrv_nfs);
    fprintf(stderr,"nfs driver init\n");
//    qemu_add_opts(&qemu_nfs_opts);
}

block_init(nfs_block_init);
