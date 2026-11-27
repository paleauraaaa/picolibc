/* Standalone Xbox guest test; link with the Xbox CRT and kernel/XAM imports. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

char __heap_start[64 * 1024] __attribute__((aligned(16)));
__asm__(".globl __heap_end\n"
        ".set __heap_end, __heap_start + 65536");

#define TEST_PATH "game:\\picolibc-chmod-test.bin"
#define TEST_DIR "game:\\picolibc-chmod-test-dir"
#define WRITE_BITS (S_IWUSR | S_IWGRP | S_IWOTH)
#define CHECK(condition, label) \
    do { \
        if (!(condition)) { \
            printf("Xbox chmod test failed: %s (errno %d)\n", label, errno); \
            return 1; \
        } \
    } while (0)

int main(void)
{
    struct stat st;
    CHECK(fchmod(-1, 0) == -1 && errno == EBADF, "invalid fd");
    CHECK(fchmod(STDOUT_FILENO, 0) == -1 && errno == ENOTSUP, "console");
    CHECK(chmod(NULL, 0) == -1 && errno == EFAULT, "null path");
    CHECK(chmod(TEST_PATH, 0) == -1 && errno == ENOENT, "missing file");

    int fd = open(TEST_PATH, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0, "create");
#ifdef TEST_CHMOD_FALLBACK
    extern int test_chmod_identity_mismatch;
    extern int test_chmod_volume_mismatch;
    test_chmod_identity_mismatch = 1;
    CHECK(fchmod(fd, 0444) == -1 && errno == ESTALE, "reopen identity mismatch");
    CHECK(fstat(fd, &st) == 0 && (st.st_mode & WRITE_BITS) == WRITE_BITS,
          "identity mismatch leaves mode unchanged");
    test_chmod_identity_mismatch = 0;
    test_chmod_volume_mismatch = 1;
    CHECK(fchmod(fd, 0444) == -1 && errno == ESTALE, "reopen volume mismatch");
    CHECK(fstat(fd, &st) == 0 && (st.st_mode & WRITE_BITS) == WRITE_BITS,
          "volume mismatch leaves mode unchanged");
    test_chmod_volume_mismatch = 0;
#endif
    CHECK(fchmod(fd, 0444) == 0, "fchmod read-only");
    CHECK(fstat(fd, &st) == 0 && !(st.st_mode & WRITE_BITS), "read-only metadata");
    CHECK(write(fd, "x", 1) == 1, "existing writable descriptor");
    CHECK(open(TEST_PATH, O_RDWR | O_TRUNC) == -1 && errno == EACCES,
          "cached writable open respects read-only");
    CHECK(open(TEST_PATH, O_WRONLY) == -1 && errno == EACCES,
          "new writable open respects read-only");
    CHECK(fstat(fd, &st) == 0 && st.st_size == 1, "read-only open did not truncate");
    CHECK(chmod(TEST_PATH, 0020) == 0, "chmod writable");
    CHECK(fstat(fd, &st) == 0 && (st.st_mode & WRITE_BITS) == WRITE_BITS,
          "writable metadata");
    CHECK(close(fd) == 0, "close");
    fd = open(TEST_PATH, O_RDONLY);
    CHECK(fd >= 0 && fchmod(fd, 0) == 0, "read-only descriptor");
    CHECK(fstat(fd, &st) == 0 && !(st.st_mode & WRITE_BITS), "zero mode");
    CHECK(fchmod(fd, 0200) == 0 && close(fd) == 0, "restore writable");
    CHECK(unlink(TEST_PATH) == 0, "unlink");

    CHECK(mkdir(TEST_DIR, 0700) == 0, "create directory");
    fd = open(TEST_DIR, O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0 && fchmod(fd, 0555) == 0, "fchmod directory");
    CHECK(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && !(st.st_mode & WRITE_BITS),
          "directory read-only metadata");
    CHECK(chmod(TEST_DIR, 0777) == 0, "chmod directory writable");
    CHECK(fstat(fd, &st) == 0 && (st.st_mode & WRITE_BITS) == WRITE_BITS,
          "directory writable metadata");
    CHECK(close(fd) == 0, "close directory");
    puts("PICO_CHMOD_PASS");
    return 0;
}
