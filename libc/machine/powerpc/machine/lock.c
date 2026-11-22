#include <sys/lock.h>

#if defined(_XBOX) && !defined(__SINGLE_THREAD)
#include <stdlib.h>
#include <xboxkrnl.h>

struct __lock {
    CRITICAL_SECTION cs;
};

struct __lock __lock___libc_recursive_mutex;

void
__xbox_libc_lock_init(void) {
    RtlInitializeCriticalSection(&__lock___libc_recursive_mutex.cs);
}

void
__retarget_lock_init(_LOCK_T *lock)
{
    /* Use the kernel pool so lock creation doesn't depend on libc malloc. */
    _LOCK_T new_lock = ExAllocatePoolWithTag(sizeof(*new_lock), 0x4c6f636b); /* Lock */
    if (!new_lock)
        abort();
    *new_lock = (struct __lock){0};
    RtlInitializeCriticalSection(&new_lock->cs);
    *lock = new_lock;
}

void
__retarget_lock_init_recursive(_LOCK_T *lock)
{
    __retarget_lock_init(lock);
}

void
__retarget_lock_close(_LOCK_T lock)
{
    ExFreePool(lock);
}

void
__retarget_lock_close_recursive(_LOCK_T lock)
{
    __retarget_lock_close(lock);
}

void __no_thread_safety_analysis
__retarget_lock_acquire(_LOCK_T lock)
{
    RtlEnterCriticalSection(&lock->cs);
}

void __no_thread_safety_analysis
__retarget_lock_acquire_recursive(_LOCK_T lock)
{
    RtlEnterCriticalSection(&lock->cs);
}

int
__xbox_lock_try_acquire(_LOCK_T lock)
{
    return RtlTryEnterCriticalSection(&lock->cs) != 0;
}

void __no_thread_safety_analysis
__retarget_lock_release(_LOCK_T lock)
{
    RtlLeaveCriticalSection(&lock->cs);
}

void __no_thread_safety_analysis
__retarget_lock_release_recursive(_LOCK_T lock)
{
    RtlLeaveCriticalSection(&lock->cs);
}

#endif /* _XBOX && !__SINGLE_THREAD */
