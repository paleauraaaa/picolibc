#include <pthread.h>

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <xboxkrnl.h>

#include "error.h"

int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr)
        return -1;

    attr->__stack_size      = 0;
    attr->__detachstate     = PTHREAD_CREATE_JOINABLE;

    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    if (!attr)
        return -1;
        
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, int stack_size) {
    if (!attr)
        return -1;

    attr->__stack_size = stack_size;
    return 0;
}

int pthread_attr_getstacksize(pthread_attr_t *attr, int *stack_size) {
    if (!attr || !stack_size)
        return -1;

    *stack_size = attr->__stack_size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
    if (!attr)
        return -1;

    attr->__detachstate = detachstate;
    return 0;
}

int pthread_attr_getdetachstate(pthread_attr_t *attr, int *detachstate) {
    if (!attr || !detachstate)
        return -1;

    *detachstate = attr->__detachstate;
    return 0;
}

typedef struct pthread_ctx_t {
    pthread_attr_t          attr;
    int                     tid;

    __pthread_start_routine start_routine;
    void*                   arg;
    void*                   ret;

    bool                    finished;
    bool                    joining;
    HANDLE                  thread_handle;
} pthread_ctx_t;

static void pthread_init_ctx(pthread_ctx_t* ctx, 
    const pthread_attr_t* attr, int tid,
                             __pthread_start_routine start_routine, void *arg)
{
    if (!ctx)
        return;

    if (!attr) {
        pthread_attr_init(&ctx->attr);
    } else {
        memcpy(&ctx->attr, attr, sizeof(*attr));
    }

    ctx->tid = tid;
    ctx->start_routine = start_routine;
    ctx->arg = arg;
    ctx->ret = NULL;
    ctx->finished = false;
    ctx->joining = false;
    ctx->thread_handle = NULL;
}

#define MAX_THREADS 256
static pthread_ctx_t *s_threads[MAX_THREADS];

// defaulted to -1, set to a thread's ID on creation.
static _Thread_local pthread_t s_self = -1;

static pthread_ctx_t *pthread_get_ctx(pthread_t thread) {
    if (thread < 0 || thread >= MAX_THREADS)
        return NULL;
    return s_threads[thread];
}

static pthread_t pthread_find_free_ctx(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!s_threads[i])
            return i;
    }
    return -1;
}

void __libc_xbox_thread_init(void) {
    // Xbox doesn't provide a gettid-equivalent function, so we hard-code
    // the main thread as tid=0 and give it a dummy context.
    s_self = 0;
    pthread_ctx_t* ctx = malloc(sizeof(pthread_ctx_t));
    s_threads[s_self] = ctx;
    pthread_init_ctx(ctx, NULL, 0, NULL, NULL);
}

static atomic_flag pthread_table_lock = ATOMIC_FLAG_INIT;

static void pthread_lock(void) {
    while (atomic_flag_test_and_set_explicit(&pthread_table_lock, memory_order_acquire))
        NtYieldExecution();
}

static void pthread_unlock(void) {
    atomic_flag_clear_explicit(&pthread_table_lock, memory_order_release);
}

enum {
    MUTEX_UNLOCKED,
    MUTEX_LOCKED,
    MUTEX_OWNER_DEAD,
    MUTEX_INCONSISTENT,
    MUTEX_NOT_RECOVERABLE,
    MUTEX_DESTROYED
};

static _Thread_local pthread_mutex_t *pthread_robust_mutexes;
static _Thread_local unsigned int pthread_mutex_owner;
static unsigned int pthread_next_mutex_owner = 1;

static void pthread_abandon_robust_mutexes(void) {
    pthread_mutex_t *mutex = pthread_robust_mutexes;
    while (mutex) {
        pthread_mutex_t *next = mutex->__next_owned;
        mutex->__owner = 0;
        mutex->__next_owned = NULL;
        mutex->__lock = MUTEX_OWNER_DEAD;
        mutex = next;
    }
    pthread_robust_mutexes = NULL;
}

pthread_t pthread_self(void) {
    return s_self;
}

static NTSTATUS NTAPI ThreadStartRoutine(PVOID StartContext) {
    pthread_t thread = (pthread_t)StartContext;
    s_self = thread;
    
    pthread_lock();
    pthread_ctx_t *ctx = pthread_get_ctx(thread);
    __pthread_start_routine start_routine = ctx->start_routine;
    void *arg = ctx->arg;
    pthread_unlock();

    void *ret = start_routine(arg);
    __libc_thread_done();

    pthread_lock();
    pthread_abandon_robust_mutexes();
    ctx->ret = ret;
    ctx->finished = true;

    NTSTATUS status = STATUS_SUCCESS;
    if (ctx->attr.__detachstate == PTHREAD_CREATE_DETACHED) {
        s_threads[thread] = NULL;
        status = NtClose(ctx->thread_handle);
        free(ctx);
        goto done;
    }

done:
    pthread_unlock();
    return status;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   __pthread_start_routine start_routine, void *arg)
{
    if (!thread)
        return -1;

    if (!start_routine)
        return -1;

    pthread_attr_t default_attr;
    if (!attr) {
        pthread_attr_init(&default_attr);
        attr = &default_attr;
    }

    pthread_ctx_t *ctx = malloc(sizeof(pthread_ctx_t));
    if (!ctx)
        return -1;

    pthread_init_ctx(ctx, attr, -1, start_routine, arg);

    pthread_lock();
    int ret = 0;
    *thread = pthread_find_free_ctx();
    if (*thread == -1) {
        ret = -1;
        goto fail;
    }
    NTSTATUS status = ExCreateThread(&ctx->thread_handle, attr->__stack_size, 
                                     &ctx->tid, NULL, ThreadStartRoutine, 
                                     (PVOID)(*thread), 0);
    if (!NT_SUCCESS(status)) {
        ret = -1;
        goto fail;
    }
    if (!ctx->thread_handle) {
        ret = -1;
        goto fail;
    }
    
    if (*thread == -1) {
        ret = -1;
        goto fail;
    }
    s_threads[*thread] = ctx;
    goto done;

fail:
    free(ctx);
    if (*thread != -1)
        s_threads[*thread] = NULL;
    goto done;
done:
    pthread_unlock();
    return ret;
}

int pthread_join(pthread_t thread, void **retval) {
    if (thread < 0 || thread >= MAX_THREADS)
        return ESRCH;

    if (thread == pthread_self())
        return EDEADLK;

    pthread_lock();
    pthread_ctx_t *ctx = pthread_get_ctx(thread);
    if (!ctx) {
        pthread_unlock();
        return ESRCH;
    }

    if (ctx->attr.__detachstate == PTHREAD_CREATE_DETACHED || ctx->joining) {
        pthread_unlock();
        return EINVAL;
    }
    ctx->joining = true;
    HANDLE thread_handle = ctx->thread_handle;
    pthread_unlock();

    NTSTATUS status = NtWaitForSingleObjectEx(thread_handle, UserMode, FALSE, NULL);
    pthread_lock();
    if (status != STATUS_SUCCESS || !ctx->finished) {
        ctx->joining = false;
        pthread_unlock();
        return NT_SUCCESS(status) ? EIO : nt_status_to_errno(status);
    }

    status = NtClose(thread_handle);
    if (!NT_SUCCESS(status)) {
        ctx->joining = false;
        pthread_unlock();
        return nt_status_to_errno(status);
    }

    if (retval)
        *retval = ctx->ret;
    s_threads[thread] = NULL;
    free(ctx);
    pthread_unlock();
    return 0;
}

int pthread_detach(pthread_t thread) {
    if (thread < 0 || thread >= MAX_THREADS)
        return -1;

    pthread_lock();
    int ret = 0;
    pthread_ctx_t *ctx = pthread_get_ctx(thread);
    if (!ctx) {
        ret = -1;
        goto done;
    }

    if (ctx->attr.__detachstate == PTHREAD_CREATE_DETACHED || ctx->joining) {
        ret = -1;
        goto done;
    }

    if (ctx->finished) {
        NTSTATUS status = NtClose(ctx->thread_handle);
        if (!NT_SUCCESS(status))
            ret = -1;
        free(ctx);
        s_threads[thread] = NULL;
        goto done;
    }

    ctx->attr.__detachstate = PTHREAD_CREATE_DETACHED;
done:
    pthread_unlock();
    return ret;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr)
        return -1;
    attr->__robustness = PTHREAD_MUTEX_STALLED;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    if (!attr)
        return -1;
    return 0;
}

int pthread_mutexattr_getrobust(const pthread_mutexattr_t *attr, int *robustness) {
    if (!attr || !robustness)
        return -1;

    *robustness = attr->__robustness;
    return 0;
}

int pthread_mutexattr_setrobust(pthread_mutexattr_t *attr, int robustness) {
    if (!attr)
        return -1;
    if (robustness != PTHREAD_MUTEX_STALLED && robustness != PTHREAD_MUTEX_ROBUST)
        return -1;

    attr->__robustness = robustness;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr) {
    if (!mutex)
        return EINVAL;

    pthread_mutexattr_t default_attr;
    if (!mutexattr) {
        pthread_mutexattr_init(&default_attr);
        mutexattr = &default_attr;
    }

    if (mutexattr->__robustness != PTHREAD_MUTEX_STALLED &&
        mutexattr->__robustness != PTHREAD_MUTEX_ROBUST)
        return EINVAL;

    mutex->__robustness = mutexattr->__robustness;
    mutex->__lock       = MUTEX_UNLOCKED;
    mutex->__owner      = 0;
    mutex->__next_owned = NULL;

    return 0;
}

static int pthread_mutex_acquire(pthread_mutex_t *mutex, bool try_only) {
    if (!mutex)
        return EINVAL;

    for (;;) {
        pthread_lock();
        int state = mutex->__lock;
        if (state == MUTEX_DESTROYED) {
            pthread_unlock();
            return EINVAL;
        }
        if (state == MUTEX_NOT_RECOVERABLE) {
            pthread_unlock();
            return ENOTRECOVERABLE;
        }
        if (state == MUTEX_UNLOCKED || state == MUTEX_OWNER_DEAD) {
            if (!pthread_mutex_owner) {
                if (!pthread_next_mutex_owner) {
                    pthread_unlock();
                    return EAGAIN;
                }
                pthread_mutex_owner = pthread_next_mutex_owner++;
            }
            mutex->__lock = state == MUTEX_OWNER_DEAD ? MUTEX_INCONSISTENT : MUTEX_LOCKED;
            mutex->__owner = pthread_mutex_owner;
            if (mutex->__robustness == PTHREAD_MUTEX_ROBUST) {
                mutex->__next_owned = pthread_robust_mutexes;
                pthread_robust_mutexes = mutex;
            }
            pthread_unlock();
            return state == MUTEX_OWNER_DEAD ? EOWNERDEAD : 0;
        }
        pthread_unlock();
        if (try_only)
            return EBUSY;
        if (sched_yield() < 0)
            return errno;
    }
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    return pthread_mutex_acquire(mutex, false);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    return pthread_mutex_acquire(mutex, true);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex)
        return EINVAL;
    pthread_lock();
    if ((mutex->__lock != MUTEX_LOCKED && mutex->__lock != MUTEX_INCONSISTENT) ||
        mutex->__owner != pthread_mutex_owner) {
        pthread_unlock();
        return EPERM;
    }
    if (mutex->__robustness == PTHREAD_MUTEX_ROBUST) {
        pthread_mutex_t **link = &pthread_robust_mutexes;
        while (*link && *link != mutex)
            link = &(*link)->__next_owned;
        if (!*link) {
            pthread_unlock();
            return EINVAL;
        }
        *link = mutex->__next_owned;
        mutex->__next_owned = NULL;
    }
    mutex->__owner = 0;
    mutex->__lock = mutex->__lock == MUTEX_INCONSISTENT ?
                    MUTEX_NOT_RECOVERABLE : MUTEX_UNLOCKED;
    pthread_unlock();
    return 0;
}

int pthread_mutex_consistent(pthread_mutex_t *mutex) {
    if (!mutex)
        return EINVAL;
    pthread_lock();
    int ret = 0;
    if (mutex->__robustness != PTHREAD_MUTEX_ROBUST ||
        mutex->__lock != MUTEX_INCONSISTENT)
        ret = EINVAL;
    else if (mutex->__owner != pthread_mutex_owner)
        ret = EPERM;
    else
        mutex->__lock = MUTEX_LOCKED;
    pthread_unlock();
    return ret;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex)
        return EINVAL;
    pthread_lock();
    int ret = 0;
    if (mutex->__lock == MUTEX_LOCKED || mutex->__lock == MUTEX_INCONSISTENT)
        ret = EBUSY;
    else if (mutex->__lock == MUTEX_DESTROYED)
        ret = EINVAL;
    else
        mutex->__lock = MUTEX_DESTROYED;
    pthread_unlock();
    return ret;
}
