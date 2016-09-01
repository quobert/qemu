/*
 * QEMU Block driver for native access to files on a Quobyte Volume
 *
 * Copyright (c) 2016 Peter Lieven <pl@kamp.de>
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

#include "qemu/osdep.h"
#include <poll.h>
#include "qemu-common.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "block/block_int.h"
#include "trace.h"
#include "qemu/iov.h"
#include "qemu/uri.h"
#include "qemu/cutils.h"
#include "sysemu/sysemu.h"

#include "quobyte.h"

typedef struct QuobyteClient {
    struct quobyte_fh *fh;
    blkcnt_t st_blocks;
    /* XXX: this is needed until fstat is in libquobyte */
    char *path;
} QuobyteClient;

static int coroutine_fn quobyte_co_readv_sync(BlockDriverState *bs,
                                     int64_t sector_num, int nb_sectors,
                                     QEMUIOVector *iov)
{
    QuobyteClient *client = bs->opaque;
    int ret;
    char *buf = g_malloc(nb_sectors * BDRV_SECTOR_SIZE);

    ret = quobyte_read(client->fh, buf, sector_num * BDRV_SECTOR_SIZE,
                       nb_sectors * BDRV_SECTOR_SIZE);

    if (ret > iov->size || ret < 0) {
        g_free(buf);
        return -EIO;
    }

    qemu_iovec_from_buf(iov, 0, buf, ret);
    g_free(buf);

    /* zero pad short reads */
    if (ret < iov->size) {
        qemu_iovec_memset(iov, ret, 0, iov->size - ret);
    }

    return 0;
}

static int coroutine_fn quobyte_co_writev_sync(BlockDriverState *bs,
                                        int64_t sector_num, int nb_sectors,
                                        QEMUIOVector *iov)
{
    QuobyteClient *client = bs->opaque;
    int ret;
    char *buf = g_malloc(nb_sectors * BDRV_SECTOR_SIZE);

    qemu_iovec_to_buf(iov, 0, buf, nb_sectors * BDRV_SECTOR_SIZE);

    ret = quobyte_write(client->fh, buf, sector_num * BDRV_SECTOR_SIZE,
                        nb_sectors * BDRV_SECTOR_SIZE);

    g_free(buf);

    if (ret != nb_sectors * BDRV_SECTOR_SIZE) {
        return -EIO;
    }

    return 0;
}

static int coroutine_fn quobyte_co_flush_sync(BlockDriverState *bs)
{
    QuobyteClient *client = bs->opaque;

    return quobyte_fsync(client->fh) ? -EIO : 0;
}

/* TODO Convert to fine grained options */
static QemuOptsList runtime_opts = {
    .name = "quobyte",
    .head = QTAILQ_HEAD_INITIALIZER(runtime_opts.head),
    .desc = {
        {
            .name = "filename",
            .type = QEMU_OPT_STRING,
            .help = "URL to the Quobyte File",
        },
        { /* end of list */ }
    },
};

//~ static void quobyte_detach_aio_context(BlockDriverState *bs)
//~ {
    //~ QuobyteClient *client = bs->opaque;
//~ 
    //~ aio_set_fd_handler(client->aio_context, quobyte_get_fd(client->context),
                       //~ false, NULL, NULL, NULL);
    //~ client->events = 0;
//~ }
//~ 
//~ static void quobyte_attach_aio_context(BlockDriverState *bs,
                                   //~ AioContext *new_context)
//~ {
    //~ QuobyteClient *client = bs->opaque;
//~ 
    //~ client->aio_context = new_context;
    //~ quobyte_set_events(client);
//~ }
//~ 
static void quobyte_client_close(QuobyteClient *client)
{
    g_free(client->path);
    if (client->fh) {
        quobyte_close(client->fh);
    }
    memset(client, 0, sizeof(QuobyteClient));
}

static void quobyte_file_close(BlockDriverState *bs)
{
    QuobyteClient *client = bs->opaque;
    quobyte_client_close(client);
}

static char *quobyteRegistry = NULL;

static int64_t quobyte_client_open(QuobyteClient *client, const char *filename,
                                   int flags, Error **errp, int open_flags)
{
    int ret = -EINVAL;
    struct stat st;
    URI *uri;

    uri = uri_parse(filename);
    if (!uri) {
        error_setg(errp, "Invalid URL specified");
        goto fail;
    }
    if (!uri->server) {
        error_setg(errp, "Invalid URL specified");
        goto fail;
    }

    if (!quobyteRegistry) {
        if (quobyte_create_adapter(uri->server)) {
            error_setg(errp, "Registration failed.");
            goto fail;
        }
        quobyteRegistry = uri->server;
    }
    //XXX: check if all connections go to the same registry

    client->path = g_strdup(uri->path);
    client->fh = quobyte_open(client->path, flags, 0600);
    if (!client->fh) {
        error_setg(errp, "Failed to open/create file: %s", strerror(errno));
        goto fail;
    }

    ret = quobyte_getattr(client->path, &st) ? -errno : 0;
    if (ret) {
        error_setg(errp, "Failed to fstat file: %s", strerror(-ret));
        goto fail;
    }

    ret = DIV_ROUND_UP(st.st_size, BDRV_SECTOR_SIZE);
    client->st_blocks = st.st_blocks;
    goto out;
fail:
out:
    uri_free(uri);
    return ret;
}

static int quobyte_file_open(BlockDriverState *bs, QDict *options, int flags,
                             Error **errp) {
    QuobyteClient *client = bs->opaque;
    int64_t ret;
    QemuOpts *opts;
    Error *local_err = NULL;

    opts = qemu_opts_create(&runtime_opts, NULL, 0, &error_abort);
    qemu_opts_absorb_qdict(opts, options, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        ret = -EINVAL;
        goto out;
    }
    ret = quobyte_client_open(client, qemu_opt_get(opts, "filename"),
                          (flags & BDRV_O_RDWR) ? O_RDWR : O_RDONLY,
                          errp, bs->open_flags);
    if (ret < 0) {
        goto out;
    }
    bs->total_sectors = ret;
    ret = 0;
out:
    qemu_opts_del(opts);
    return ret;
}

static QemuOptsList quobyte_create_opts = {
    .name = "nfs-create-opts",
    .head = QTAILQ_HEAD_INITIALIZER(quobyte_create_opts.head),
    .desc = {
        {
            .name = BLOCK_OPT_SIZE,
            .type = QEMU_OPT_SIZE,
            .help = "Virtual disk size"
        },
        { /* end of list */ }
    }
};

static int quobyte_file_create(const char *url, QemuOpts *opts, Error **errp)
{
    int ret = 0;
    int64_t total_size = 0;
    QuobyteClient *client = g_new0(QuobyteClient, 1);

    /* Read out options */
    total_size = ROUND_UP(qemu_opt_get_size_del(opts, BLOCK_OPT_SIZE, 0),
                          BDRV_SECTOR_SIZE);

    ret = quobyte_client_open(client, url, O_CREAT, errp, 0);
    if (ret < 0) {
        goto out;
    }
    ret = quobyte_ftruncate(client->fh, total_size) ? errno : 0;
out:
    quobyte_client_close(client);
    g_free(client);
    return ret;
}

static int64_t quobyte_get_allocated_file_size(BlockDriverState *bs)
{
    QuobyteClient *client = bs->opaque;
    struct stat st;

    if (bdrv_is_read_only(bs) &&
        !(bs->open_flags & BDRV_O_NOCACHE)) {
        return client->st_blocks * 512;
    }

    if (quobyte_getattr(client->path, &st)) {
        return -errno;
    }
    
    return st.st_blocks * 512;
}

static int quobyte_file_truncate(BlockDriverState *bs, int64_t offset)
{
    QuobyteClient *client = bs->opaque;
    return quobyte_ftruncate(client->fh, offset) ? errno : 0;
}


static BlockDriver bdrv_quobyte = {
    .format_name                    = "quobyte",
    .protocol_name                  = "quobyte",

    .instance_size                  = sizeof(QuobyteClient),
    .bdrv_needs_filename            = true,
    .create_opts                    = &quobyte_create_opts,

    .bdrv_has_zero_init             = bdrv_has_zero_init_1,
    .bdrv_get_allocated_file_size   = quobyte_get_allocated_file_size,
    .bdrv_truncate                  = quobyte_file_truncate,

    .bdrv_file_open                 = quobyte_file_open,
    .bdrv_close                     = quobyte_file_close,
    .bdrv_create                    = quobyte_file_create,

    .bdrv_co_readv                  = quobyte_co_readv_sync,
    .bdrv_co_writev                 = quobyte_co_writev_sync,
    .bdrv_co_flush_to_disk          = quobyte_co_flush_sync,
};

static void quobyte_block_init(void)
{
    bdrv_register(&bdrv_quobyte);
}

block_init(quobyte_block_init);
