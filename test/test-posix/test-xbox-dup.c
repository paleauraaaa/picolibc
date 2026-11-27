/* Standalone Xbox guest test; link with the Xbox CRT and kernel/XAM imports. */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char __heap_start[64 * 1024] __attribute__((aligned(16)));
__asm__(".globl __heap_end\n.set __heap_end, __heap_start + 65536");

#define PATH "game:\\picolibc-dup-test.bin"
#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "dup test failed: %s (errno %d)\n", label, errno); \
        return 1; \
    } \
} while (0)

static void *duplicate_worker(void *arg)
{
    int fd = *(int *)arg;
    for (int i = 0; i < 100; ++i) {
        int copy = dup(fd);
        struct stat st;
        if (copy < 0 || fstat(copy, &st) != 0 || close(copy) != 0)
            return (void *)1;
    }
    return NULL;
}

int main(void)
{
    struct stat st;
    char buf[8];
    int saved_stdout = dup(STDOUT_FILENO);
    CHECK(saved_stdout == STDIN_FILENO, "duplicate console into lowest free fd");
    CHECK(fstat(saved_stdout, &st) == 0 && S_ISCHR(st.st_mode), "duplicated console metadata");
    CHECK(lseek(saved_stdout, 0, SEEK_CUR) == -1 && errno == ESPIPE, "console is not seekable");
    CHECK(dup2(STDOUT_FILENO, STDOUT_FILENO) == STDOUT_FILENO, "dup2 same fd");
    CHECK(close(saved_stdout) == 0, "close console duplicate");

    int fd = open(PATH, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd == STDIN_FILENO, "open uses lowest free descriptor");
    CHECK(write(fd, "abcdef", 6) == 6 && lseek(fd, 0, SEEK_SET) == 0, "prepare file");
    int copy = dup(fd);
    CHECK(copy == 3, "dup uses lowest free descriptor");
    CHECK(read(copy, buf, 2) == 2 && memcmp(buf, "ab", 2) == 0, "read duplicate");
    CHECK(lseek(fd, 0, SEEK_CUR) == 2, "duplicate shares offset");
    CHECK(lseek(fd, 4, SEEK_SET) == 4 && read(copy, buf, 2) == 2 &&
          memcmp(buf, "ef", 2) == 0, "seek shared by duplicate");
    CHECK(pread(copy, buf, 1, 0) == 1 && lseek(fd, 0, SEEK_CUR) == 6,
          "pread preserves shared offset");

    int independent = open(PATH, O_RDWR);
    CHECK(independent >= 0 && lseek(independent, 0, SEEK_CUR) == 0,
          "independent open has independent offset");
    CHECK(dup2(fd, independent) == independent && lseek(independent, 0, SEEK_CUR) == 6,
          "dup2 replaces independent open");
    CHECK(lseek(independent, 1, SEEK_SET) == 1 && lseek(copy, 0, SEEK_CUR) == 1,
          "replacement shares offset");
    CHECK(dup2(-1, independent) == -1 && errno == EBADF &&
          lseek(independent, 0, SEEK_CUR) == 1, "invalid source preserves target");
    CHECK(dup2(fd, -1) == -1 && errno == EBADF, "invalid target");
    CHECK(dup2(fd, 1024) == -1 && errno == EBADF, "target beyond table");
    CHECK(dup(-1) == -1 && errno == EBADF, "invalid source");
    pthread_t workers[2];
    for (int i = 0; i < 2; ++i)
        CHECK(pthread_create(&workers[i], NULL, duplicate_worker, &copy) == 0,
              "start concurrent dup worker");
    for (int i = 0; i < 2; ++i) {
        void *result;
        CHECK(pthread_join(workers[i], &result) == 0 && result == NULL,
              "join concurrent dup worker");
    }
    CHECK(close(fd) == 0 && read(copy, buf, 1) == 1 && buf[0] == 'b',
          "duplicate survives closing original");

    int fds[1024];
    int n = 0;
    for (; n < 1024; ++n) {
        fds[n] = dup(copy);
        if (fds[n] < 0)
            break;
    }
    CHECK(n < 1024 && errno == EMFILE, "dup reports full table");
    CHECK(dup2(copy, fds[n - 1]) == fds[n - 1], "dup2 works with full table");
    for (int i = 0; i < n; ++i)
        CHECK(close(fds[i]) == 0, "close table duplicates");

    saved_stdout = dup(STDOUT_FILENO);
    CHECK(saved_stdout >= 0 && fflush(stdout) == 0, "save console output");
    CHECK(dup2(copy, STDOUT_FILENO) == STDOUT_FILENO &&
          fstat(STDOUT_FILENO, &st) == 0 && S_ISREG(st.st_mode),
          "redirect stdout descriptor");
    int output_ok = write(STDOUT_FILENO, "X", 1) == 1 &&
                    fputs("Y", stdout) >= 0 && fflush(stdout) == 0;
    CHECK(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO, "restore stdout");
    CHECK(output_ok && pread(copy, buf, 4, 0) == 4 &&
          memcmp(buf, "abXY", 4) == 0, "stdout writes follow redirected descriptor");
    int saved_stderr = dup(STDERR_FILENO);
    CHECK(saved_stderr >= 0 && dup2(copy, STDERR_FILENO) == STDERR_FILENO,
          "redirect stderr descriptor");
    output_ok = fputs("Z", stderr) >= 0 && fflush(stderr) == 0;
    CHECK(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO, "restore stderr");
    CHECK(output_ok && pread(copy, buf, 5, 0) == 5 &&
          memcmp(buf, "abXYZ", 5) == 0, "stderr writes follow redirected descriptor");
    CHECK(close(STDOUT_FILENO) == 0 &&
          dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO,
          "dup2 restores closed stdout");
    CHECK(close(saved_stdout) == 0 && close(saved_stderr) == 0 &&
          close(independent) == 0 && close(copy) == 0,
          "close duplicate descriptors");
    CHECK(unlink(PATH) == 0, "remove fixture");

    int directory = open("game:\\", O_RDONLY | O_DIRECTORY);
    CHECK(directory >= 0, "open directory");
    int directory_copy = dup(directory);
    CHECK(directory_copy >= 0 && close(directory) == 0, "duplicate directory survives close");
    DIR *stream = fdopendir(directory_copy);
    CHECK(stream != NULL, "fdopendir accepts duplicated directory");
    errno = EDOM;
    CHECK(dirfd(stream) == directory_copy && errno == EDOM,
          "dirfd returns the stream descriptor without changing errno");
    CHECK(fstatat(dirfd(stream), ".", &st, 0) == 0 && S_ISDIR(st.st_mode),
          "dirfd descriptor supports directory-relative operations");
    errno = 0;
    CHECK(readdir(stream) != NULL || errno == 0, "read duplicated directory");
    CHECK(dirfd(stream) == directory_copy, "dirfd remains stable after readdir");
    CHECK(closedir(stream) == 0, "close duplicated directory stream");
    CHECK(fstat(directory_copy, &st) == -1 && errno == EBADF,
          "closedir closes dirfd descriptor");
    stream = opendir("game:\\");
    CHECK(stream != NULL, "opendir game root");
    int stream_fd = dirfd(stream);
    CHECK(stream_fd >= 0 && fstat(stream_fd, &st) == 0 && S_ISDIR(st.st_mode),
          "dirfd returns opendir descriptor");
    CHECK(closedir(stream) == 0 && fstat(stream_fd, &st) == -1 && errno == EBADF,
          "closedir closes opendir descriptor");
    puts("PICO_DUP_PASS");
    return 0;
}
