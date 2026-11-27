/* Standalone Xbox guest test; link with the Xbox CRT and kernel/XAM imports. */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

static pthread_mutex_t mutex;
static pthread_mutex_t stalled_mutex;
static atomic_int ready;
static atomic_int release_owner;
static atomic_int waiter_ready;
static int counter;

static int
check(int condition, const char *message)
{
    if (!condition)
        printf("pthread mutex test failed: %s\n", message);
    return condition;
}

static void *
count_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        int ret = pthread_mutex_lock(&mutex);
        if (ret)
            return (void *)1;
        counter++;
        if (pthread_mutex_unlock(&mutex))
            return (void *)1;
    }
    return NULL;
}

static void *
dead_owner(void *arg)
{
    pthread_mutex_t *owned = arg ? arg : &mutex;
    if (pthread_mutex_lock(owned))
        return (void *)1;
    atomic_store_explicit(&ready, 1, memory_order_release);
    while (!atomic_load_explicit(&release_owner, memory_order_acquire))
        ;
    return NULL;
}

static void *
recover_worker(void *arg)
{
    (void)arg;
    atomic_store_explicit(&waiter_ready, 1, memory_order_release);
    int ret = pthread_mutex_lock(&mutex);
    if (ret != EOWNERDEAD)
        return (void *)1;
    if (pthread_mutex_consistent(&mutex) || pthread_mutex_unlock(&mutex))
        return (void *)1;
    return NULL;
}

static void *
wrong_owner(void *arg)
{
    pthread_mutex_t *owned = arg ? arg : &mutex;
    return pthread_mutex_unlock(owned) == EPERM ? NULL : (void *)1;
}

static void *
die_during_recovery(void *arg)
{
    (void)arg;
    return pthread_mutex_lock(&mutex) == EOWNERDEAD ? NULL : (void *)1;
}

static int
join_ok(pthread_t thread)
{
    void *result = NULL;
    return pthread_join(thread, &result) == 0 && result == NULL;
}

int
main(void)
{
    pthread_t workers[4];
    pthread_t owner, waiter;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    pthread_condattr_t cond_attr = { 0 };
    pthread_key_t key = 123;
    pthread_mutexattr_t stub_attr = { 0 };
    if (!check(pthread_mutexattr_settype(&stub_attr, PTHREAD_MUTEX_NORMAL) == ENOSYS, "mutex type unsupported") ||
        !check(pthread_condattr_init(&cond_attr) == ENOSYS, "condattr init unsupported") ||
        !check(pthread_condattr_setclock(&cond_attr, 0) == ENOSYS, "condattr clock unsupported") ||
        !check(pthread_condattr_destroy(&cond_attr) == ENOSYS, "condattr destroy unsupported") ||
        !check(pthread_cond_init(&cond, &cond_attr) == ENOSYS, "cond init unsupported") ||
        !check(pthread_cond_signal(&cond) == ENOSYS, "cond signal unsupported") ||
        !check(pthread_cond_broadcast(&cond) == ENOSYS, "cond broadcast unsupported") ||
        !check(pthread_cond_wait(&cond, &mutex) == ENOSYS, "cond wait unsupported") ||
        !check(pthread_cond_timedwait(&cond, &mutex, NULL) == ENOSYS, "cond timedwait unsupported") ||
        !check(pthread_cond_destroy(&cond) == ENOSYS, "cond destroy unsupported") ||
        !check(pthread_key_create(&key, NULL) == ENOSYS && key == 123, "key create unsupported") ||
        !check(pthread_key_delete(key) == ENOSYS, "key delete unsupported") ||
        !check(pthread_setspecific(key, &key) == ENOSYS, "setspecific unsupported"))
        return 1;
    errno = 0;
    if (!check(pthread_getspecific(key) == NULL && errno == ENOSYS, "getspecific unsupported"))
        return 1;
    if (!check(pthread_mutex_init(&mutex, NULL) == 0, "default mutex init") ||
        !check(pthread_mutex_lock(&mutex) == 0, "default mutex lock") ||
        !check(pthread_mutex_trylock(&mutex) == EBUSY, "trylock busy") ||
        !check(pthread_mutex_destroy(&mutex) == EBUSY, "destroy busy"))
        return 1;
    if (!check(pthread_create(&owner, NULL, wrong_owner, NULL) == 0, "create non-owner") ||
        !check(join_ok(owner), "reject non-owner unlock") ||
        !check(pthread_mutex_unlock(&mutex) == 0, "default mutex unlock"))
        return 1;
    for (int i = 0; i < 4; i++)
        if (!check(pthread_create(&workers[i], NULL, count_worker, NULL) == 0, "create counter worker"))
            return 1;
    for (int i = 0; i < 4; i++)
        if (!check(join_ok(workers[i]), "join counter worker"))
            return 1;
    if (!check(counter == 8000, "mutual exclusion") ||
        !check(pthread_mutex_destroy(&mutex) == 0, "destroy default mutex"))
        return 1;

    atomic_store(&release_owner, 1);
    if (!check(pthread_mutex_init(&stalled_mutex, NULL) == 0, "stalled mutex init") ||
        !check(pthread_create(&owner, NULL, dead_owner, &stalled_mutex) == 0, "create stalled owner") ||
        !check(join_ok(owner), "join stalled owner") ||
        !check(pthread_create(&owner, NULL, wrong_owner, &stalled_mutex) == 0, "reuse stalled owner slot") ||
        !check(join_ok(owner), "reject reused owner") ||
        !check(pthread_mutex_trylock(&stalled_mutex) == EBUSY, "stalled mutex remains locked"))
        return 1;

    pthread_mutexattr_t attr;
    if (!check(pthread_mutexattr_init(&attr) == 0, "mutex attribute init") ||
        !check(pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0, "set robust") ||
        !check(pthread_mutex_init(&mutex, &attr) == 0, "robust mutex init"))
        return 1;

    atomic_store(&ready, 0);
    atomic_store(&release_owner, 0);
    atomic_store(&waiter_ready, 0);
    if (!check(pthread_create(&owner, NULL, dead_owner, NULL) == 0, "create owner"))
        return 1;
    while (!atomic_load_explicit(&ready, memory_order_acquire))
        ;
    if (!check(pthread_create(&waiter, NULL, recover_worker, NULL) == 0, "create waiter"))
        return 1;
    while (!atomic_load_explicit(&waiter_ready, memory_order_acquire))
        ;
    atomic_store_explicit(&release_owner, 1, memory_order_release);
    if (!check(join_ok(owner) && join_ok(waiter), "recover after owner exits") ||
        !check(pthread_mutex_lock(&mutex) == 0, "lock after recovery") ||
        !check(pthread_mutex_unlock(&mutex) == 0, "unlock after recovery"))
        return 1;

    atomic_store(&ready, 0);
    atomic_store(&release_owner, 1);
    if (!check(pthread_create(&owner, NULL, dead_owner, NULL) == 0, "create second owner") ||
        !check(join_ok(owner), "join second owner") ||
        !check(pthread_create(&owner, NULL, die_during_recovery, NULL) == 0,
               "create recovery owner") ||
        !check(join_ok(owner), "owner dies during recovery") ||
        !check(pthread_mutex_trylock(&mutex) == EOWNERDEAD, "trylock dead owner") ||
        !check(pthread_mutex_unlock(&mutex) == 0, "unlock inconsistent mutex") ||
        !check(pthread_mutex_lock(&mutex) == ENOTRECOVERABLE, "unrecoverable mutex") ||
        !check(pthread_mutex_destroy(&mutex) == 0, "destroy unrecoverable mutex"))
        return 1;

    printf("pthread mutex test passed\n");
    return 0;
}
