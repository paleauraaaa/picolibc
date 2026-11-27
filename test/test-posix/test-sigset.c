/* SPDX-License-Identifier: BSD-3-Clause */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            printf("SIGSET_FAIL: line %d (errno %d)\n", __LINE__, errno);  \
            return 1;                                                       \
        }                                                                   \
    } while (0)

int
main(void)
{
    sigset_t set;
    CHECK((sigemptyset)(&set) == 0);
    CHECK(sigismember(&set, SIGINT) == 0);
    CHECK((sigaddset)(&set, SIGINT) == 0);
    CHECK((sigaddset)(&set, SIGTERM) == 0);
    CHECK((sigaddset)(&set, _NSIG - 1) == 0);
    CHECK(sigismember(&set, SIGINT) == 1);
    CHECK(sigismember(&set, SIGTERM) == 1);
    CHECK(sigismember(&set, _NSIG - 1) == 1);
    unsigned char before[sizeof set];
    memcpy(before, &set, sizeof set);
    errno = 0;
    CHECK((sigaddset)(&set, 0) == -1 && errno == EINVAL);
    errno = 0;
    CHECK((sigaddset)(&set, -1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK((sigaddset)(&set, _NSIG) == -1 && errno == EINVAL);
    CHECK(memcmp(before, &set, sizeof set) == 0);
    CHECK((sigemptyset)(&set) == 0);
    CHECK(sigismember(&set, SIGINT) == 0);
    CHECK(sigismember(&set, _NSIG - 1) == 0);
    printf("SIGSET_PASS\n");
    return 0;
}
