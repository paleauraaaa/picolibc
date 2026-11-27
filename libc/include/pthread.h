#ifndef _PTHREAD_H_
#define _PTHREAD_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stddef.h>

_BEGIN_STD_C

typedef int pthread_t;

#define PTHREAD_SCOPE_PROCESS       0
#define PTHREAD_SCOPE_SYSTEM        1

#define PTHREAD_CREATE_JOINABLE     0
#define PTHREAD_CREATE_DETACHED     1

#define PTHREAD_STACK_MIN           16384

typedef struct pthread_attr_t {
    size_t __stack_size;
    int __detachstate;
} pthread_attr_t;

#define PTHREAD_MUTEX_STALLED       0
#define PTHREAD_MUTEX_ROBUST        1
#define PTHREAD_MUTEX_NORMAL        0

typedef struct pthread_mutexattr_t {
    int __robustness;
} pthread_mutexattr_t;

typedef struct pthread_mutex_t {
    int                     __robustness;
    int                     __lock;
    unsigned int            __owner;
    struct pthread_mutex_t *__next_owned;
} pthread_mutex_t;

/* Placeholder types for the currently unsupported condition variables and keys. */
typedef struct pthread_cond_t {
    int __placeholder;
} pthread_cond_t;

typedef struct pthread_condattr_t {
    int __placeholder;
} pthread_condattr_t;

typedef unsigned int pthread_key_t;

#define PTHREAD_COND_INITIALIZER { 0 }

#define PTHREAD_MUTEX_INITIALIZER              \
{                                              \
    .__robustness   = PTHREAD_MUTEX_STALLED,   \
    .__lock         = 0,                       \
    .__owner        = 0,                       \
    .__next_owned   = NULL                     \
}

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stack_size);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stack_size);

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
int pthread_attr_getdetachstate(pthread_attr_t *attr, int *detachstate);

pthread_t pthread_self(void);
typedef void *(*__pthread_start_routine)(void*);
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   __pthread_start_routine start_routine, void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_getrobust(const pthread_mutexattr_t *attr, int *robustness);
int pthread_mutexattr_setrobust(pthread_mutexattr_t *attr, int robustness);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_consistent(pthread_mutex_t *mutex);

struct timespec;
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime);
int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id);

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

void __libc_thread_done(void);

_END_STD_C

#endif /* _PTHREAD_H_ */
