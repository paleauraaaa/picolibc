/* Standalone Xbox guest test; link with the Xbox CRT and kernel/XAM imports. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>

static volatile sig_atomic_t calls;

static void
handler(int sig)
{
    if (sig == SIGTERM) {
        ++calls;
        if (calls == 1)
            raise(sig);
    }
}

#define CHECK(condition, label)                                  \
    do {                                                         \
        if (!(condition)) {                                      \
            printf("XBOX_SIGNAL_FAIL: %s (errno %d)\n", label, errno); \
            return 1;                                            \
        }                                                        \
    } while (0)

int
main(void)
{
    errno = 0;
    CHECK(signal(-1, handler) == SIG_ERR && errno == EINVAL, "invalid signal");
    errno = 0;
    CHECK(signal(0, handler) == SIG_ERR && errno == EINVAL, "zero signal");
    errno = 0;
    CHECK(signal(_NSIG, handler) == SIG_ERR && errno == EINVAL, "signal limit");
    errno = 0;
    CHECK(raise(-1) == -1 && errno == EINVAL, "invalid raise");
    errno = 0;
    CHECK(raise(0) == -1 && errno == EINVAL, "zero raise");
    CHECK(signal(SIGTERM, handler) == SIG_DFL, "register handler");
    CHECK(raise(SIGTERM) == 0 && calls == 2, "deliver recursive signal");
    CHECK(signal(SIGTERM, SIG_IGN) == handler, "return previous handler");
    CHECK(raise(SIGTERM) == 0 && calls == 2, "ignore signal");
    CHECK(signal(SIGTERM, SIG_DFL) == SIG_IGN, "restore default");
    printf("XBOX_SIGNAL_PASS\n");
    raise(SIGABRT);
    printf("XBOX_SIGNAL_FAIL: default action returned\n");
    return 1;
}
