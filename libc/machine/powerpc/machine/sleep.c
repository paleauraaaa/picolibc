#include <time.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>

#include <xboxkrnl.h>

#include "error.h"

int nanosleep(const struct timespec *duration, struct timespec *rem) {
    if (!duration) {
        errno = EINVAL;
        return -1;
    }

    if (duration->tv_sec < 0 || duration->tv_nsec < 0 || duration->tv_nsec >= 1000000000) {
        errno = EINVAL;
        return -1;
    }

    int64_t fractional_ticks = (duration->tv_nsec + 99) / 100;
    if (duration->tv_sec > (INT64_MAX - fractional_ticks) / 10000000) {
        errno = EOVERFLOW;
        return -1;
    }

    int64_t ticks = duration->tv_sec * 10000000LL + fractional_ticks;
    if (ticks == 0) {
        if (sched_yield() < 0)
            return -1;
    } else {
        LARGE_INTEGER interval;
        interval.QuadPart = -ticks;
        NTSTATUS status = KeDelayExecutionThread(UserMode, 0, &interval);
        if (status != STATUS_SUCCESS) {
            errno = nt_status_to_errno(status);
            return -1;
        }
    }

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}