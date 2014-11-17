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

#define DEBUG (0)

#if CONFIG_COROUTINE_POOL > 0
enum {
    /* Maximum free pool size prevents holding too many freed coroutines */
    POOL_MAX_SIZE = 64,
};

/** Free list to speed up creation */
struct CoRoutinePool {
    Coroutine *ptrs[POOL_MAX_SIZE];
    unsigned int size;
    unsigned int nextfree;
    QemuMutex lock;
    bool enabled;
};

static __thread struct CoRoutinePool ThreadCoPool;
static struct CoRoutinePool GlobalCoPool;
#endif

Coroutine *qemu_coroutine_create(CoroutineEntry *entry)
{
    Coroutine *co = NULL;
#if CONFIG_COROUTINE_POOL > 0
	if (!ThreadCoPool.enabled) {
		qemu_mutex_lock(&GlobalCoPool.lock);
		if (GlobalCoPool.size) {
			co = GlobalCoPool.ptrs[GlobalCoPool.nextfree];
			GlobalCoPool.size--;
			GlobalCoPool.nextfree--;
			GlobalCoPool.nextfree &= (POOL_MAX_SIZE - 1);
		}
		qemu_mutex_unlock(&GlobalCoPool.lock);
	} else {
		if (ThreadCoPool.size) {
			co = ThreadCoPool.ptrs[ThreadCoPool.nextfree];
			ThreadCoPool.size--;
			ThreadCoPool.nextfree--;
			ThreadCoPool.nextfree &= (POOL_MAX_SIZE - 1);
		}
	}
#endif

    if (!co) {
        co = qemu_coroutine_new();
    }
#if DEBUG > 1
    printf("qemu_coroutine_create thread %lx co %p local %d\n", pthread_self(), co, ThreadCoPool.enabled);
#endif
    co->entry = entry;
    co->caller = NULL;
    QTAILQ_INIT(&co->co_queue_wakeup);
    return co;
}

static void coroutine_delete(Coroutine *co)
{
#if DEBUG > 1
    printf("coroutine_delete thread %lx co %p local %d\n", pthread_self(), co, ThreadCoPool.enabled);
#endif
#if CONFIG_COROUTINE_POOL > 0
	if (!ThreadCoPool.enabled) {
		Coroutine *delete_co = NULL;
		qemu_mutex_lock(&GlobalCoPool.lock);
		GlobalCoPool.nextfree++;
		GlobalCoPool.nextfree &= (POOL_MAX_SIZE - 1);
		if (GlobalCoPool.size == POOL_MAX_SIZE) {
			delete_co = GlobalCoPool.ptrs[GlobalCoPool.nextfree];
		} else {
			GlobalCoPool.size++;
		}
		GlobalCoPool.ptrs[GlobalCoPool.nextfree] = co;
		qemu_mutex_unlock(&GlobalCoPool.lock);
		if (delete_co) {
			qemu_coroutine_delete(delete_co);
		}
	} else {
		ThreadCoPool.nextfree++;
		ThreadCoPool.nextfree &= (POOL_MAX_SIZE - 1);
		if (ThreadCoPool.size == POOL_MAX_SIZE) {
			qemu_coroutine_delete(ThreadCoPool.ptrs[ThreadCoPool.nextfree]);
		} else {
			ThreadCoPool.size++;
		}
		ThreadCoPool.ptrs[ThreadCoPool.nextfree] = co;
	}
#else
	qemu_coroutine_delete(co);
#endif
}

static void __attribute__((constructor)) coroutine_pool_init(void)
{
#if DEBUG
    printf("coroutine_pool_init %lx pool %p coroutine pool %d\n", pthread_self(), &ThreadCoPool, CONFIG_COROUTINE_POOL);
#endif
#if CONFIG_COROUTINE_POOL > 0
    qemu_mutex_init(&GlobalCoPool.lock);
#endif
}

void coroutine_pool_enable_local(void)
{
#if DEBUG
    printf("coroutine_pool_enable %lx pool %p\n", pthread_self(), &ThreadCoPool);
#endif
#if CONFIG_COROUTINE_POOL > 0
    ThreadCoPool.enabled = true;
#endif
}

void coroutine_pool_cleanup_local(void)
{
#if DEBUG
    printf("coroutine_pool_cleanup %lx pool %p\n", pthread_self(), &ThreadCoPool);
#endif
#if CONFIG_COROUTINE_POOL > 0
    while (ThreadCoPool.size) {
        qemu_coroutine_delete(qemu_coroutine_create(NULL));
    }
#endif
}

static void __attribute__((destructor)) coroutine_pool_destroy(void)
{
#if DEBUG
    printf("coroutine_pool_destroy %lx pool %p\n", pthread_self(), &ThreadCoPool);
#endif
#if CONFIG_COROUTINE_POOL > 0
    while (GlobalCoPool.size) {
        qemu_coroutine_delete(qemu_coroutine_create(NULL));
    }
    qemu_mutex_destroy(&GlobalCoPool.lock);
#endif
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
