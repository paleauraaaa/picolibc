/* Standalone Xbox guest test; link with the Xbox CRT and kernel/XAM imports. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

unsigned int _tls_index;
/* Give the standalone test a small heap for Picolibc's fallback sbrk. */
char __heap_start[64 * 1024] __attribute__((aligned(16)));
__asm__(".globl __heap_end\n"
        ".set __heap_end, __heap_start + 65536");

#define POSIX_DIR "/game/picolibc-path-xenia-dir"
#define POSIX_CHILD POSIX_DIR "/child"
#define FILE_NAME "picolibc-path-xenia-test.bin"
#define POSIX_DOT_PATH POSIX_DIR "/child/../child/" FILE_NAME
#define NATIVE_DOT_PATH "game:\\picolibc-path-xenia-dir\\child\\..\\child\\" FILE_NAME

#define CHECK(condition, label)                                      \
    do {                                                              \
        if (!(condition)) {                                           \
            printf("Xbox path test failed: %s (errno %d)\n", label, errno); \
            return 1;                                                 \
        }                                                             \
    } while (0)

int
main(void)
{
    char buffer[5];
    char cwd[128];
    int dirfd = open(POSIX_DIR, O_CREAT | O_RDWR | O_DIRECTORY, 0);
    CHECK(dirfd >= 0, "create POSIX directory");
    CHECK(close(dirfd) == 0, "close POSIX directory");
    dirfd = open(POSIX_CHILD, O_CREAT | O_RDWR | O_DIRECTORY, 0);
    CHECK(dirfd >= 0, "create child directory");
    CHECK(close(dirfd) == 0, "close child directory");

    CHECK(chdir("child") == -1, "relative chdir requires initial absolute cwd");
    CHECK(getcwd(cwd, sizeof(cwd)) == NULL && errno == ENOENT,
          "getcwd requires initialized cwd");
    CHECK(chdir(POSIX_DIR "/child/..") == 0, "normalize absolute chdir");
    CHECK(getcwd(cwd, sizeof(cwd)) == cwd, "getcwd after absolute chdir");
    CHECK(strcmp(cwd, POSIX_DIR) == 0, "absolute chdir removes dotdot");
    CHECK(chdir("./child/../child") == 0, "normalize relative chdir");
    CHECK(getcwd(cwd, sizeof(cwd)) == cwd, "getcwd after relative chdir");
    CHECK(strcmp(cwd, POSIX_CHILD) == 0, "relative chdir appends to cwd");
    CHECK(chdir("..") == 0, "relative chdir to parent");
    CHECK(getcwd(cwd, sizeof(cwd)) == cwd, "getcwd after parent chdir");
    CHECK(strcmp(cwd, POSIX_DIR) == 0, "parent path is normalized");
    CHECK(chdir("../..") == 0, "relative chdir clamps at volume root");
    CHECK(getcwd(cwd, sizeof(cwd)) == cwd, "getcwd at volume root");
    CHECK(strcmp(cwd, "/game") == 0, "parent path cannot escape volume");
    CHECK(chdir(POSIX_CHILD) == 0, "restore child directory");
    char short_cwd[2];
    CHECK(getcwd(short_cwd, sizeof(short_cwd)) == NULL && errno == ERANGE,
          "getcwd rejects undersized buffer");

    int fd = open("./" FILE_NAME, O_CREAT | O_TRUNC | O_RDWR, 0);
    CHECK(fd >= 0, "open relative path");
    CHECK(write(fd, "paths", sizeof(buffer)) == sizeof(buffer), "write");
    CHECK(close(fd) == 0, "close after write");

    fd = openat(AT_FDCWD, "../child/./" FILE_NAME, O_RDONLY);
    CHECK(fd >= 0, "openat relative to cwd");
    CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer), "read cwd-relative path");
    CHECK(memcmp(buffer, "paths", sizeof(buffer)) == 0, "verify cwd-relative path");
    CHECK(close(fd) == 0, "close cwd-relative path");

    fd = openat(-1, POSIX_DOT_PATH, O_RDONLY);
    CHECK(fd >= 0, "absolute path ignores dirfd");
    CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer), "read POSIX path");
    CHECK(memcmp(buffer, "paths", sizeof(buffer)) == 0, "verify POSIX path");
    CHECK(close(fd) == 0, "close absolute path");

    fd = open(NATIVE_DOT_PATH, O_RDONLY);
    CHECK(fd >= 0, "open native path with dotdot");
    CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer), "read native path");
    CHECK(memcmp(buffer, "paths", sizeof(buffer)) == 0, "verify native path");
    CHECK(close(fd) == 0, "close native path");

    puts("PICO_PATH_PASS");
    return 0;
}
