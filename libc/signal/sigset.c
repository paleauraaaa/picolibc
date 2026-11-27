/* SPDX-License-Identifier: BSD-3-Clause */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>

int
(sigaddset)(sigset_t *set, int sig)
{
    if (sig <= 0 || sig >= _NSIG) {
        errno = EINVAL;
        return -1;
    }
    return __sigaddset(set, sig);
}

int
(sigemptyset)(sigset_t *set)
{
    return __sigemptyset(set);
}
