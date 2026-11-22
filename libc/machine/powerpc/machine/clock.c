#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <time.h>

#include <xboxkrnl.h>

int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    if (clock_id != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }

    uint64_t ticks = KeQueryInterruptTime();
    tp->tv_sec = (time_t)(ticks / 10000000);
    tp->tv_nsec = (long)((ticks % 10000000) * 100);
    return 0;
}
