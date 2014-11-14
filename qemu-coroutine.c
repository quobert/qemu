/*
 * QEMU coroutines
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi    <stefanha@linux.vnet.ibm.com>
 *  Kevin Wolf         <kwolf@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "trace.h"
#include "qemu-common.h"
#include "qemu/thread.h"
#include "block/coroutine.h"
#include "block/coroutine_int.h"

enum {
    /* Maximum free pool size prevents holding too many freed coroutines */
    POOL_MAX_SIZE = 64,
};

/** Free list to speed up creation */
static __thread struct CoRoutinePool {
    Coroutine *ptrs[POOL_MAX_SIZE];
    unsigned int size;
    unsigned int nextfree;
} CoPool;

Coroutine *qemu_coroutine_create(CoroutineEntry *entry)
{
    Coroutine *co = NULL;
    if (CONFIG_COROUTINE_POOL) {
        if (CoPool.size) {
            co = CoPool.ptrs[CoPool.nextfree];
            CoPool.size--;
            CoPool.nextfree--;
            CoPool.nextfree &= (POOL_MAX_SIZE - 1);
        }
    }

    if (!co) {
        co = qemu_coroutine_new();
    }

    co->entry = entry;
    QTAILQ_INIT(&co->co_queue_wakeup);
    return co;
}

static void coroutine_delete(Coroutine *co)
{
    if (CONFIG_COROUTINE_POOL) {
        CoPool.nextfree++;
        CoPool.nextfree &= (POOL_MAX_SIZE - 1);
        if (CoPool.size == POOL_MAX_SIZE) {
            qemu_coroutine_delete(CoPool.ptrs[CoPool.nextfree]);
        } else {
            CoPool.size++;
        }
        co->caller = NULL;
        CoPool.ptrs[CoPool.nextfree] = co;
    } else {
        qemu_coroutine_delete(co);
    }
}

static void __attribute__((constructor)) coroutine_pool_init(void)
{
}

void __attribute__((destructor)) coroutine_pool_cleanup(void)
{
    printf("coroutine_pool_cleanup %lx pool %p\n", pthread_self(), &CoPool);
    while (CoPool.size) {
        qemu_coroutine_delete(qemu_coroutine_create(NULL));
    }
}

static void coroutine_swap(Coroutine *from, Coroutine *to)
{
    CoroutineAction ret;

    ret = qemu_coroutine_switch(from, to, COROUTINE_YIELD);

    qemu_co_queue_run_restart(to);

    switch (ret) {
    case COROUTINE_YIELD:
        return;
    case COROUTINE_TERMINATE:
        trace_qemu_coroutine_terminate(to);
        coroutine_delete(to);
        return;
    default:
        abort();
    }
}

void qemu_coroutine_enter(Coroutine *co, void *opaque)
{
    Coroutine *self = qemu_coroutine_self();

    trace_qemu_coroutine_enter(self, co, opaque);

    if (co->caller) {
        fprintf(stderr, "Co-routine re-entered recursively\n");
        abort();
    }

    co->caller = self;
    co->entry_arg = opaque;
    coroutine_swap(self, co);
}

void coroutine_fn qemu_coroutine_yield(void)
{
    Coroutine *self = qemu_coroutine_self();
    Coroutine *to = self->caller;

    trace_qemu_coroutine_yield(self, to);

    if (!to) {
        fprintf(stderr, "Co-routine is yielding to no one\n");
        abort();
    }

    self->caller = NULL;
    coroutine_swap(self, to);
}
