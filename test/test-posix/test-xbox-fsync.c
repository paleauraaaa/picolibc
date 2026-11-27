/* Standalone Xbox guest test; link with the Xbox CRT and kernel/XAM imports. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

char __heap_start[64 * 1024] __attribute__((aligned(16)));
__asm__(".globl __heap_end\n"
        ".set __heap_end, __heap_start + 65536");

#define TEST_PATH "game:\\picolibc-fsync-test.bin"
#define CHECK(condition, label) \
    do { \
        if (!(condition)) { \
            printf("Xbox fsync test failed: %s (errno %d)\n", label, errno); \
            return 1; \
        } \
    } while (0)

int main(void)
{
    CHECK(fsync(-1) == -1 && errno == EBADF, "invalid fd");
    CHECK(fsync(STDOUT_FILENO) == -1 && errno == EINVAL, "console");
    int fd = open(TEST_PATH, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0, "create");
    CHECK(write(fd, "data", 4) == 4, "write");
    CHECK(fsync(fd) == 0, "flush");
    int copy = dup(fd);
    CHECK(copy >= 0 && close(fd) == 0 && fsync(copy) == 0, "duplicate");
#ifdef TEST_FSYNC_INTERCEPT
    extern int test_fsync_calls;
    extern unsigned long test_fsync_status;
    int calls = test_fsync_calls;
    test_fsync_status = 0xc000007f;
    CHECK(fsync(copy) == -1 && errno == ENOSPC, "disk full");
    CHECK(test_fsync_calls == calls + 1, "native call");
    test_fsync_status = 0xc0000022;
    CHECK(fsync(copy) == -1 && errno == EACCES, "access denied");
    test_fsync_status = 0;
#endif
    CHECK(close(copy) == 0, "close");
    CHECK(fsync(copy) == -1 && errno == EBADF, "closed fd");
#ifdef TEST_FSYNC_INTERCEPT
    int calls_before_invalid = test_fsync_calls;
    CHECK(fsync(-1) == -1 && errno == EBADF &&
          fsync(STDOUT_FILENO) == -1 && errno == EINVAL &&
          test_fsync_calls == calls_before_invalid, "invalid handles not flushed");
    fd = open(TEST_PATH, O_RDONLY);
    CHECK(fd >= 0, "open read-only");
    calls = test_fsync_calls;
    test_fsync_status = 0xc0000022;
    CHECK(fsync(fd) == -1 && errno == EACCES && test_fsync_calls == calls + 1,
          "read-only native status");
    test_fsync_status = 0;
    CHECK(close(fd) == 0, "close read-only");
    fd = open("game:\\", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0, "open directory");
    calls = test_fsync_calls;
    test_fsync_status = 0xc0000022;
    CHECK(fsync(fd) == -1 && errno == EACCES && test_fsync_calls == calls + 1,
          "directory native status");
    test_fsync_status = 0;
    CHECK(close(fd) == 0, "close directory");
#endif
    CHECK(unlink(TEST_PATH) == 0, "unlink");
    puts("PICO_FSYNC_PASS");
    return 0;
}
