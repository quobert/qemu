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
    bool has_zero_init;
} nfsclient;

typedef struct NFSTask {
    int status;
    int complete;
    QEMUIOVector *iov;
    Coroutine *co;
} NFSTask;

static void nfs_process_read(void *arg);
static void nfs_process_write(void *arg);

static void nfs_set_events(nfsclient *client)
{
    int ev;
    /* We always register a read handler.  */
    printf("set events enter\n");
    ev = POLLIN;
    ev |= nfs_which_events(client->context);
    if (ev != client->events) {
        qemu_aio_set_fd_handler(nfs_get_fd(client->context),
                      nfs_process_read,
                      (ev & POLLOUT) ? nfs_process_write : NULL,
                      client);

    }
    printf("set events done\n");
    client->events = ev;
}

static void nfs_process_read(void *arg)
{
    nfsclient *client = arg;
    printf("nfs_process_read enter service %16lx\n",(uintptr_t) arg);
    int ret = nfs_service(client->context, POLLIN);
    printf("nfs_process_read leave service %16lx exit %d\n",(uintptr_t) arg,ret);
    nfs_set_events(client);
}

static void nfs_process_write(void *arg)
{
    nfsclient *client = arg;
    printf("nfs_process_write enter service %16lx\n",(uintptr_t) arg);
    nfs_service(client->context, POLLOUT);
    printf("nfs_process_write leave service %16lx\n",(uintptr_t) arg);
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
    Task->complete = 1;
    Task->status = status;
    if (Task->status > 0 && Task->iov) {
		printf("qemu_iovec_from_buf status len %d\n",(int) status);
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
    
   	printf("nfs_readv pointers bs = %16lx bs->opaque = %16lx client->context = %16lx\n",(uintptr_t)bs,(uintptr_t)bs->opaque,(uintptr_t)client->context);
    
    nfs_co_init_task(client, &Task);
    Task.iov = iov;
    fprintf(stderr,"nfs_co_readv %lld %d %d %16lx\n",(long long int) sector_num, nb_sectors, (int) iov->size, (uintptr_t) iov->iov[0].iov_base);
    if (nfs_pread_async(client->context, client->fh, sector_num * BDRV_SECTOR_SIZE,
                        nb_sectors * BDRV_SECTOR_SIZE, nfs_co_generic_cb, &Task) != 0) {
        return -EIO;
    }

    while (!Task.complete) {
		printf("nfs_readv: nfs_set_events\n");
        nfs_set_events(client);
        qemu_coroutine_yield();
    }
    
    if (Task.status != nb_sectors * BDRV_SECTOR_SIZE) {
        return -EIO;
    }
    
//    fprintf(stderr,"read %d bytes\n", Task.status);
	return 0;
}

//return nfs_pwrite_async(nfs, nfsfh, nfsfh->offset, count, buf, cb, private_data);

static int coroutine_fn nfs_co_writev(BlockDriverState *bs,
                                        int64_t sector_num, int nb_sectors,
                                        QEMUIOVector *iov)
{
    nfsclient *client = bs->opaque;
    struct NFSTask Task;
    char *buf = NULL;
    
    nfs_co_init_task(client, &Task);
    
    buf = g_malloc(nb_sectors * BDRV_SECTOR_SIZE);
    qemu_iovec_to_buf(iov, 0, buf, nb_sectors * BDRV_SECTOR_SIZE);
    
    fprintf(stderr,"nfs_co_writev %lld %d\n",(long long int) sector_num, nb_sectors);
    
    if (nfs_pwrite_async(client->context, client->fh, sector_num * BDRV_SECTOR_SIZE,
                         nb_sectors * BDRV_SECTOR_SIZE, buf, nfs_co_generic_cb, &Task) != 0) {
        g_free(buf);
        return -EIO;
    }

    while (!Task.complete) {
		printf("nfs_writev: set events\n");
        nfs_set_events(client);
        qemu_coroutine_yield();
    }
    
    g_free(buf);
    
    if (Task.status != nb_sectors * BDRV_SECTOR_SIZE) {
        return -EIO;
    }
    
    bs->total_sectors = MAX(bs->total_sectors, sector_num + nb_sectors);
    fprintf(stderr,"write ok\n");
	return 0;
}

static int coroutine_fn nfs_co_flush(BlockDriverState *bs)
{
	fprintf(stderr,"nfs_file_flush\n");
    nfsclient *client = bs->opaque;
    struct NFSTask Task;
    
    nfs_co_init_task(client, &Task);
    
    if (nfs_fsync_async(client->context, client->fh, nfs_co_generic_cb, &Task) != 0) {
        return -EIO;
    }

    while (!Task.complete) {
		printf("nfs set events\n");
        nfs_set_events(client);
        qemu_coroutine_yield();
    }
    
    if (Task.status != 0) {
        return -EIO;
    }

	return 0;
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
    nfsclient *client = bs->opaque;
    if (client->context) {
	    fprintf(stderr,"nfs_file_close fd = %d\n",nfs_get_fd(client->context));
		if (client->fh) {
			nfs_close(client->context, client->fh);
		}
		fprintf(stderr,"nfs_destroy\n");
		qemu_aio_set_fd_handler(nfs_get_fd(client->context), NULL, NULL, NULL);
		nfs_destroy_context(client->context);
    }
    printf("nfs close done!\n");
    memset(client, 0, sizeof(nfsclient));
}

static int nfs_file_open_common(BlockDriverState *bs, QDict *options, int flags,
                         int open_flags, Error **errp)
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
	fprintf(stderr,"nfs_file_open_common: %s\n",filename);
    
    client->context = nfs_init_context();
	
	printf("nfs pointers bs = %16lx bs->opaque = %16lx client->context = %16lx\n",(uintptr_t)bs,(uintptr_t)bs->opaque,(uintptr_t)client->context);
	
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
	
	printf("mount done\n");

    if (open_flags & O_CREAT) {
		if (nfs_creat(client->context, file, 0600, &client->fh) != 0) {
			fprintf(stderr, "Failed to create file : %s\n",
							nfs_get_error(client->context));
			ret = -EINVAL;
			goto fail;
		}
	} else {
		open_flags = (flags & BDRV_O_RDWR) ? O_RDWR : O_RDONLY;
		if (nfs_open(client->context, file, open_flags, &client->fh) != 0) {
			fprintf(stderr, "Failed to open file : %s\n",
							nfs_get_error(client->context));
			ret = -EINVAL;
			goto fail;
		}
	}

	if (nfs_fstat(client->context, client->fh, &st) != 0) {
		fprintf(stderr, "Failed to fstat source file\n");
		ret = -EIO;
		goto fail;
	}
	
	printf("stat done\n");

    bs->total_sectors = st.st_size / BDRV_SECTOR_SIZE;
    client->has_zero_init = S_ISREG(st.st_mode);
 printf("blocksize %d\n",(int) st.st_blksize);
 printf("blockcnt %d\n",(int) st.st_blocks);

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
    printf("nfs_file_open_common ret = %d\n",ret);
    return ret;
}

static int nfs_file_open(BlockDriverState *bs, QDict *options, int flags,
                         Error **errp) {
    return nfs_file_open_common(bs, options, flags, 0, errp);							 
}

static int nfs_file_create(const char *filename, QEMUOptionParameter *options,
                         Error **errp)
{
    fprintf(stderr,"nfs_file_create: %s\n",filename);
    int ret = 0;
    int64_t total_size = 0;
    BlockDriverState *bs;
    nfsclient *client = NULL;
    QDict *bs_options;

    bs = bdrv_new("");

    /* Read out options */
    while (options && options->name) {
        if (!strcmp(options->name, "size")) {
            total_size = options->value.n / BDRV_SECTOR_SIZE;
        }
        options++;
    }

    bs->opaque = g_malloc0(sizeof(struct nfsclient));
    client = bs->opaque;

    bs_options = qdict_new();
    qdict_put(bs_options, "filename", qstring_from_str(filename));
    ret = nfs_file_open_common(bs, bs_options, 0, O_CREAT, NULL);
    QDECREF(bs_options);
    if (ret != 0) {
        goto out;
    }
    printf("total_sectors %d total_size %d\n",(int)bs->total_sectors,(int)total_size);
    ret = nfs_ftruncate(client->context, client->fh, total_size * BDRV_SECTOR_SIZE);
    if (ret != 0) {
        ret = -ENOSPC;;
    }

    ret = 0;
out:
    nfs_file_close(bs);
    g_free(bs->opaque);
    bs->opaque = NULL;
    bdrv_unref(bs);
    printf("bdrv_create ret = %d\n",ret);
    return ret;
}

static int nfs_has_zero_init(BlockDriverState *bs)
{
    nfsclient *client = bs->opaque;
    return client->has_zero_init;
}

static BlockDriver bdrv_nfs = {
    .format_name     = "nfs",
    .protocol_name   = "nfs",

    .instance_size   = sizeof(nfsclient),
    .bdrv_needs_filename = true,
    .bdrv_has_zero_init = nfs_has_zero_init,
    .bdrv_file_open  = nfs_file_open,
    .bdrv_close      = nfs_file_close,
    .bdrv_create     = nfs_file_create,

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
