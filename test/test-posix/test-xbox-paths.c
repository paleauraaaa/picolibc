/* Standalone Xbox guest test; link with the Xbox CRT and kernel/XAM imports. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <windef.h>
#include <xboxkrnl.h>

struct file_descriptor;
extern struct file_descriptor *file_descriptor(int fd);

/* Give the standalone test a small heap for Picolibc's fallback sbrk. */
char __heap_start[64 * 1024] __attribute__((aligned(16)));
__asm__(".globl __heap_end\n"
        ".set __heap_end, __heap_start + 65536");

#define POSIX_DIR "/game/picolibc-path-xenia-dir"
#define POSIX_CHILD POSIX_DIR "/child"
#define FILE_NAME "picolibc-path-xenia-test.bin"
#define POSIX_DOT_PATH POSIX_DIR "/child/../child/" FILE_NAME
#define NATIVE_DOT_PATH "game:\\picolibc-path-xenia-dir\\child\\..\\child\\" FILE_NAME
/* The harness removes this fixture because Xbox rmdir is not implemented. */
#define MKDIR_FIXTURE POSIX_CHILD "/picolibc-mkdir-fixture"

#define CHECK(condition, label)                                      \
    do {                                                              \
        if (!(condition)) {                                           \
            printf("Xbox path test failed: %s (errno %d)\n", label, errno); \
            return 1;                                                 \
        }                                                             \
    } while (0)

static int
matches_filetime(struct timespec time, LONGLONG ticks)
{
    if (ticks == 0)
        return time.tv_sec == 0 && time.tv_nsec == 0;
    return time.tv_sec == ticks / 10000000LL - 11644473600LL &&
           time.tv_nsec == (ticks % 10000000LL) * 100;
}

static void *
shared_open_worker(void *arg)
{
    for (int i = 0; i < 32; ++i) {
        int fd = open(POSIX_DOT_PATH, O_RDWR);
        if (fd < 0)
            return (void *)1;
        char ch;
        int ok = file_descriptor(fd) == arg &&
                 read(fd, &ch, 1) == 1 && ch == 'p';
        if (close(fd) != 0 || !ok)
            return (void *)1;
    }
    return NULL;
}

int
main(void)
{
    char buffer[5];
    char cwd[128];
    struct stat file_stat, other_stat;

    memset(&file_stat, 0xcc, sizeof(file_stat));
    memcpy(&other_stat, &file_stat, sizeof(file_stat));
    errno = 0;
    CHECK(fstat(-1, &file_stat) == -1 && errno == EBADF, "fstat invalid descriptor");
    CHECK(memcmp(&file_stat, &other_stat, sizeof(file_stat)) == 0,
          "fstat leaves buffer unchanged on error");
    errno = 0;
    CHECK(fstat(STDIN_FILENO, &file_stat) == -1 && errno == EBADF, "fstat stdin");
    CHECK(fstat(STDOUT_FILENO, &file_stat) == 0 && S_ISCHR(file_stat.st_mode),
          "fstat stdout");
    CHECK(fstat(STDERR_FILENO, &file_stat) == 0 && S_ISCHR(file_stat.st_mode),
          "fstat stderr");
    CHECK(fstatat(STDOUT_FILENO, "", &file_stat, AT_EMPTY_PATH) == 0 &&
          S_ISCHR(file_stat.st_mode), "fstatat empty stdout path");
    CHECK(fstatat(-1, "", &file_stat, AT_EMPTY_PATH) == -1 && errno == EBADF,
          "fstatat empty path rejects invalid descriptor");
    errno = 0;
    CHECK(fstatat(AT_FDCWD, "relative", &file_stat, 0) == -1 && errno == ENOENT,
          "fstatat without current directory");
    CHECK(fstatat(AT_FDCWD, "", &file_stat, AT_EMPTY_PATH) == -1 && errno == ENOENT,
          "fstatat empty path without current directory");
    CHECK(fstatat(-1, POSIX_DIR, &file_stat, 0x40000000) == -1 && errno == EINVAL,
          "fstatat rejects unknown flags");
    CHECK(fstatat(-1, NULL, &file_stat, 0) == -1 && errno == EFAULT,
          "fstatat null path");
    CHECK(fstatat(-1, POSIX_DIR, NULL, 0) == -1 && errno == EFAULT,
          "fstatat null buffer");
    CHECK(mkdir(NULL, 0777) == -1 && errno == EFAULT, "mkdir null path");
    CHECK(mkdirat(-1, "", 0777) == -1 && errno == ENOENT,
          "mkdirat empty path");
    CHECK(mkdir("relative-mkdir", 0777) == -1 && errno == ENOENT,
          "mkdir requires initialized current directory");
    CHECK(fdopendir(-1) == NULL && errno == EBADF, "fdopendir invalid descriptor");
    CHECK(fdopendir(STDOUT_FILENO) == NULL && errno == EBADF,
          "fdopendir stdout");
    CHECK(opendir("/game/picolibc-missing-dir-for-opendir") == NULL && errno == ENOENT,
          "opendir missing directory");
    int dirfd = open(POSIX_DIR, O_CREAT | O_RDWR | O_DIRECTORY, 0);
    CHECK(dirfd >= 0, "create POSIX directory");
    memset(&file_stat, 0xcc, sizeof(file_stat));
    CHECK(fstat(dirfd, &file_stat) == 0 && S_ISDIR(file_stat.st_mode), "fstat directory");
    CHECK((file_stat.st_mode & S_IXUSR) != 0, "directory is searchable");
    CHECK(fstatat(dirfd, ".", &other_stat, 0) == 0 && S_ISDIR(other_stat.st_mode) &&
          other_stat.st_dev == file_stat.st_dev && other_stat.st_ino == file_stat.st_ino,
          "fstatat relative directory");
    CHECK(fstatat(dirfd, "", &other_stat, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) == 0 &&
          other_stat.st_dev == file_stat.st_dev && other_stat.st_ino == file_stat.st_ino,
          "fstatat empty directory path");
    CHECK(fstatat(dirfd, "", &other_stat, 0) == -1 && errno == ENOENT,
          "fstatat rejects empty path");
    memcpy(&other_stat, &file_stat, sizeof(file_stat));
    CHECK(fstatat(dirfd, ".", &other_stat, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOSYS,
          "fstatat no-follow is unsupported");
    CHECK(memcmp(&file_stat, &other_stat, sizeof(file_stat)) == 0,
          "fstatat leaves buffer unchanged on error");
    int shared_dirfd = open(POSIX_DIR, O_RDWR | O_DIRECTORY);
    CHECK(shared_dirfd >= 0 && shared_dirfd != dirfd, "open another directory descriptor");
    DIR *stream = fdopendir(dirfd);
    CHECK(stream != NULL, "fdopendir directory");
    CHECK(fstat(dirfd, &other_stat) == 0 && S_ISDIR(other_stat.st_mode),
          "fdopendir retains descriptor");
    CHECK(closedir(stream) == 0, "closedir descriptor stream");
    CHECK(fstat(dirfd, &other_stat) == -1 && errno == EBADF,
          "closedir closes descriptor");
    CHECK(fstat(shared_dirfd, &other_stat) == 0 && close(shared_dirfd) == 0,
          "closedir preserves another directory descriptor");
    dirfd = open(POSIX_DIR, O_WRONLY | O_DIRECTORY);
    CHECK(dirfd >= 0, "open write-only directory");
    CHECK(fdopendir(dirfd) == NULL && errno == EBADF,
          "fdopendir rejects write-only descriptor");
    CHECK(fstat(dirfd, &other_stat) == 0 && close(dirfd) == 0,
          "failed fdopendir preserves descriptor");
    stream = opendir(POSIX_DIR);
    CHECK(stream != NULL && closedir(stream) == 0, "opendir and closedir");
    CHECK(stat(POSIX_DIR, &file_stat) == 0 && S_ISDIR(file_stat.st_mode),
          "stat directory");
    errno = 0;
    CHECK(lstat(POSIX_DIR, &file_stat) == -1 && errno == ENOSYS,
          "lstat is unsupported");
    dirfd = open(POSIX_CHILD, O_CREAT | O_RDWR | O_DIRECTORY, 0);
    CHECK(dirfd >= 0, "create child directory");
    CHECK(close(dirfd) == 0, "close child directory");
    CHECK(rename(POSIX_CHILD, POSIX_DIR "/renamed-child") == 0,
          "rename directory");
    CHECK(stat(POSIX_DIR "/renamed-child", &other_stat) == 0 &&
          S_ISDIR(other_stat.st_mode), "renamed directory is accessible");
    CHECK(rename(POSIX_DIR "/renamed-child", POSIX_CHILD) == 0,
          "restore renamed directory");

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
    CHECK(fstatat(AT_FDCWD, "", &other_stat, AT_EMPTY_PATH) == 0 &&
          S_ISDIR(other_stat.st_mode), "fstatat empty path for current directory");
    char short_cwd[2];
    CHECK(getcwd(short_cwd, sizeof(short_cwd)) == NULL && errno == ERANGE,
          "getcwd rejects undersized buffer");

    int fd = open("./" FILE_NAME, O_CREAT | O_TRUNC | O_RDWR, 0);
    CHECK(fd >= 0, "open relative path");
    CHECK(fdopendir(fd) == NULL && errno == ENOTDIR,
          "fdopendir rejects regular file");
    errno = 0;
    CHECK(fstat(fd, NULL) == -1 && errno == EFAULT, "fstat null buffer");
    CHECK(fstat(fd, &file_stat) == 0 && file_stat.st_size == 0, "fstat empty file");
    CHECK(write(fd, "paths", sizeof(buffer)) == sizeof(buffer), "write");
    CHECK(mkdir(MKDIR_FIXTURE, 0000) == 0, "mkdir absolute directory");
    CHECK(stat(MKDIR_FIXTURE, &other_stat) == 0 && S_ISDIR(other_stat.st_mode) &&
          (other_stat.st_mode & S_IRWXU) == S_IRWXU, "mkdir mode is synthesized");
    CHECK(mkdir(MKDIR_FIXTURE, 0777) == -1 && errno == EEXIST,
          "mkdir existing directory");
    CHECK(mkdir(FILE_NAME, 0777) == -1 && errno == EEXIST,
          "mkdir existing file");
    CHECK(mkdirat(-1, "relative-mkdir", 0777) == -1 && errno == EBADF,
          "mkdirat invalid descriptor");
    CHECK(mkdirat(fd, "relative-mkdir", 0777) == -1 && errno == ENOTDIR,
          "mkdirat file descriptor");
    CHECK(mkdirat(-1, MKDIR_FIXTURE "/absolute", 0700) == 0,
          "mkdirat absolute path ignores descriptor");
    int mkdir_dirfd = open(MKDIR_FIXTURE, O_RDONLY | O_DIRECTORY);
    CHECK(mkdir_dirfd >= 0, "open mkdirat base");
    CHECK(mkdirat(mkdir_dirfd, "nested/", 0755) == 0,
          "mkdirat relative path with trailing slash");
    CHECK(mkdirat(mkdir_dirfd, "nested/../other", 0755) == 0,
          "mkdirat normalizes relative path");
    CHECK(mkdirat(mkdir_dirfd, "missing/child", 0755) == -1 && errno == ENOENT,
          "mkdirat missing parent");
    CHECK(mkdirat(mkdir_dirfd, "nested", 0755) == -1 && errno == EEXIST,
          "mkdirat existing directory");
    CHECK(mkdirat(mkdir_dirfd, ".", 0755) == -1 && errno == EEXIST,
          "mkdirat existing dot path");
    CHECK(close(mkdir_dirfd) == 0, "close mkdirat base");
    CHECK(mkdir("picolibc-mkdir-fixture/cwd", 0777) == 0,
          "mkdir relative to current directory");
    CHECK(mkdir("game:\\picolibc-path-xenia-dir\\child\\picolibc-mkdir-fixture\\native",
                0777) == 0, "mkdir native absolute path");
    int exhausted_fds[1024];
    int exhausted_count = 0;
    while (exhausted_count < 1024) {
        int extra_fd = open(FILE_NAME, O_RDWR);
        if (extra_fd < 0)
            break;
        exhausted_fds[exhausted_count++] = extra_fd;
    }
    CHECK(exhausted_count < 1024 && errno == EMFILE, "exhaust descriptor table");
    CHECK(mkdir(MKDIR_FIXTURE "/exhausted", 0755) == 0,
          "mkdir does not require a free descriptor");
    for (int i = 0; i < exhausted_count; ++i)
        CHECK(close(exhausted_fds[i]) == 0, "close exhausted descriptor");
    CHECK(stat(MKDIR_FIXTURE "/absolute", &other_stat) == 0 &&
          S_ISDIR(other_stat.st_mode) &&
          stat(MKDIR_FIXTURE "/nested", &other_stat) == 0 &&
          S_ISDIR(other_stat.st_mode) &&
          stat(MKDIR_FIXTURE "/other", &other_stat) == 0 &&
          S_ISDIR(other_stat.st_mode) &&
          stat(MKDIR_FIXTURE "/cwd", &other_stat) == 0 &&
          S_ISDIR(other_stat.st_mode) &&
          stat(MKDIR_FIXTURE "/native", &other_stat) == 0 &&
          S_ISDIR(other_stat.st_mode), "mkdir creates directories");
    char long_name[97];
    memset(long_name, 'q', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    int long_fd = open(long_name, O_CREAT | O_EXCL | O_RDWR, 0);
    CHECK(long_fd >= 0 && close(long_fd) == 0, "create long directory entry");

    DIR *streams[2] = { opendir(POSIX_CHILD), opendir(POSIX_CHILD) };
    CHECK(streams[0] != NULL && streams[1] != NULL, "open independent streams");
    int found[2] = { 0, 0 };
    int done[2] = { 0, 0 };
    for (int i = 0; i < 10000 && (!done[0] || !done[1]); ++i) {
        int which = i % 2;
        if (done[which])
            continue;
        errno = 0;
        struct dirent *entry = readdir(streams[which]);
        if (entry == NULL) {
            CHECK(errno == 0, "readdir end of stream");
            done[which] = 1;
        } else if (strcmp(entry->d_name, FILE_NAME) == 0) {
            CHECK(entry->d_type == DT_REG && !(found[which] & 1),
                  "readdir regular file");
            CHECK(fstat(fd, &other_stat) == 0 && entry->d_ino == other_stat.st_ino,
                  "readdir file ID");
            found[which] |= 1;
        } else if (strcmp(entry->d_name, long_name) == 0) {
            CHECK(entry->d_type == DT_REG && !(found[which] & 2),
                  "readdir long name");
            found[which] |= 2;
        }
    }
    CHECK(done[0] && done[1] && found[0] == 3 && found[1] == 3,
          "independent streams enumerate all entries");
    errno = EDOM;
    CHECK(readdir(streams[0]) == NULL && errno == EDOM,
          "readdir preserves errno at end");
    CHECK(closedir(streams[0]) == 0 && closedir(streams[1]) == 0,
          "close independent streams");

    stream = opendir(POSIX_DIR);
    CHECK(stream != NULL, "open parent directory stream");
    int found_child = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(stream);
        if (entry == NULL)
            break;
        if (strcmp(entry->d_name, "child") == 0) {
            CHECK(entry->d_type == DT_DIR, "readdir directory type");
            CHECK(stat(POSIX_CHILD, &other_stat) == 0 &&
                  entry->d_ino == other_stat.st_ino, "readdir directory ID");
            found_child = 1;
        }
    }
    CHECK(errno == 0 && found_child, "readdir finds child directory");
    CHECK(closedir(stream) == 0, "close parent directory stream");

    stream = opendir(POSIX_CHILD);
    CHECK(stream != NULL, "open stream for batched records");
    memset(stream->buf, 0, sizeof(stream->buf));
    ULONG length = strlen(FILE_NAME);
    ULONG next = (64 + length + 7) & ~7U;
    memcpy(stream->buf, &next, sizeof(next));
    memcpy(stream->buf + 60, &length, sizeof(length));
    memcpy(stream->buf + 64, FILE_NAME, length);
    length = strlen(long_name);
    memcpy(stream->buf + next + 60, &length, sizeof(length));
    memcpy(stream->buf + next + 64, long_name, length);
    stream->offset = 0;
    stream->count = next + 64 + length;
    entry = readdir(stream);
    CHECK(entry != NULL && strcmp(entry->d_name, FILE_NAME) == 0 &&
          entry->d_type == DT_REG, "first batched directory record");
    entry = readdir(stream);
    CHECK(entry != NULL && strcmp(entry->d_name, long_name) == 0 &&
          entry->d_type == DT_REG, "second batched directory record");
    next = 65;
    memcpy(stream->buf, &next, sizeof(next));
    stream->offset = 0;
    stream->count = 200;
    CHECK(readdir(stream) == NULL && errno == EIO,
          "readdir rejects malformed record offset");
    next = 0;
    length = 256;
    memcpy(stream->buf, &next, sizeof(next));
    memcpy(stream->buf + 60, &length, sizeof(length));
    stream->offset = 0;
    stream->count = 320;
    CHECK(readdir(stream) == NULL && errno == ENAMETOOLONG,
          "readdir rejects unrepresentable name");
    const char missing_entry[] = "picolibc-readdir-missing-entry";
    length = sizeof(missing_entry) - 1;
    memcpy(stream->buf + 60, &length, sizeof(length));
    memcpy(stream->buf + 64, missing_entry, length);
    stream->offset = 0;
    stream->count = 64 + length;
    CHECK(readdir(stream) == NULL && errno == ENOENT,
          "readdir reports file ID lookup errors");
    CHECK(closedir(stream) == 0, "close batched record stream");
    CHECK(unlink(long_name) == 0, "remove long directory entry");

    dirfd = open(POSIX_CHILD, O_RDONLY | O_DIRECTORY);
    CHECK(dirfd >= 0 && (stream = fdopendir(dirfd)) != NULL,
          "open stream for invalid descriptor test");
    CHECK(close(dirfd) == 0, "invalidate stream descriptor");
    CHECK(readdir(stream) == NULL && errno == EBADF,
          "readdir rejects closed descriptor");
    CHECK(closedir(stream) == -1 && errno == EBADF,
          "closedir releases invalid stream");
    memset(&file_stat, 0xcc, sizeof(file_stat));
    CHECK(fstat(fd, &file_stat) == 0, "fstat written file");
    CHECK(S_ISREG(file_stat.st_mode) && file_stat.st_size == sizeof(buffer),
          "fstat file type and size");
    CHECK(fstatat(fd, "", &other_stat, AT_EMPTY_PATH) == 0 &&
          other_stat.st_dev == file_stat.st_dev && other_stat.st_ino == file_stat.st_ino &&
          other_stat.st_size == file_stat.st_size, "fstatat empty file path");
    CHECK(fstatat(fd, FILE_NAME, &other_stat, 0) == -1 && errno == ENOTDIR,
          "fstatat rejects file as directory");
    CHECK(fstatat(-1, POSIX_DOT_PATH, &other_stat, 0) == 0 &&
          other_stat.st_ino == file_stat.st_ino, "fstatat absolute path ignores dirfd");
    CHECK(fstatat(AT_FDCWD, FILE_NAME, &other_stat, 0) == 0 &&
          other_stat.st_ino == file_stat.st_ino, "fstatat relative to current directory");
    CHECK(fstatat(-1, FILE_NAME, &other_stat, 0) == -1 && errno == EBADF,
          "fstatat rejects invalid dirfd");
    CHECK((file_stat.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) ==
          (S_IRUSR | S_IWUSR), "fstat regular file permissions");
    CHECK(file_stat.st_blksize == 512 && file_stat.st_blocks >= 1,
          "fstat allocation");
    CHECK(file_stat.st_nlink == 0 && file_stat.st_uid == 0 &&
          file_stat.st_gid == 0 && file_stat.st_rdev == 0,
          "fstat unavailable metadata");

    int second_fd = open(POSIX_DOT_PATH, O_RDWR);
    CHECK(second_fd >= 0 && second_fd != fd, "reopen gets distinct descriptor");
    CHECK(file_descriptor(second_fd) == file_descriptor(fd), "reopen shares file");
    CHECK(lseek(second_fd, 0, SEEK_CUR) == 0, "reopen starts at offset zero");
    CHECK(read(second_fd, buffer, 1) == 1 && buffer[0] == 'p',
          "reopened descriptor reads independently");
    CHECK(lseek(fd, 0, SEEK_CUR) == 5, "reopen preserves original offset");
    int readonly_fd = open("./" FILE_NAME, O_RDONLY);
    CHECK(readonly_fd >= 0, "open with different access");
    CHECK(file_descriptor(readonly_fd) != file_descriptor(fd),
          "different access does not share file");
    CHECK(write(readonly_fd, "x", 1) == -1 && errno == EBADF,
          "different access remains read-only");
    CHECK(close(readonly_fd) == 0, "close different access");
    errno = 0;
    CHECK(open("./" FILE_NAME, O_CREAT | O_EXCL | O_RDWR, 0) == -1 &&
          errno == EEXIST, "exclusive reopen fails");
    int trunc_fd = open("./" FILE_NAME, O_TRUNC | O_RDWR);
    CHECK(trunc_fd >= 0, "reopen with truncate");
    CHECK(file_descriptor(trunc_fd) == file_descriptor(fd),
          "truncate reuses compatible file");
    CHECK(fstat(trunc_fd, &other_stat) == 0 && other_stat.st_size == 0,
          "reopen truncates file");
    CHECK(pwrite(trunc_fd, "paths", sizeof(buffer), 0) == sizeof(buffer),
          "restore file after reopen truncation");
    CHECK(close(trunc_fd) == 0, "close truncated descriptor");
    CHECK(stat("./" FILE_NAME, &other_stat) == 0 && S_ISREG(other_stat.st_mode),
          "stat regular file");
    CHECK(close(fd) == 0, "close first shared descriptor");
    CHECK(read(fd, buffer, 1) == -1 && errno == EBADF,
          "closed shared descriptor is invalid");
    fd = second_fd;
    CHECK(lseek(fd, 5, SEEK_SET) == 5, "remaining descriptor stays open");

    CHECK(pread(fd, buffer, 3, 1) == 3 && memcmp(buffer, "ath", 3) == 0,
          "pread uses explicit offset");
    CHECK(lseek(fd, 0, SEEK_CUR) == 5, "pread preserves file offset");
    CHECK(pwrite(fd, "XY", 2, 1) == 2, "pwrite uses explicit offset");
    CHECK(lseek(fd, 0, SEEK_CUR) == 5, "pwrite preserves file offset");
    CHECK(pread(fd, buffer, sizeof(buffer), 0) == sizeof(buffer) &&
          memcmp(buffer, "pXYhs", sizeof(buffer)) == 0, "verify pwrite");
    CHECK(pread(fd, buffer, sizeof(buffer), 5) == 0, "pread at EOF");
    CHECK(pread(fd, buffer, 1, -1) == -1 && errno == EINVAL, "pread negative offset");
    CHECK(pwrite(fd, buffer, 1, -1) == -1 && errno == EINVAL, "pwrite negative offset");
    CHECK(pwrite(fd, "at", 2, 1) == 2, "restore file");
    CHECK(ftruncate(fd, -1) == -1 && errno == EINVAL, "ftruncate negative length");
    CHECK(ftruncate(fd, 3) == 0, "shrink file");
    CHECK(lseek(fd, 0, SEEK_CUR) == 5, "shrink preserves file offset");
    CHECK(pread(fd, buffer, sizeof(buffer), 0) == 3 &&
          memcmp(buffer, "pat", 3) == 0, "verify shrunk file");
    CHECK(ftruncate(fd, 5) == 0, "extend file");
    CHECK(lseek(fd, 0, SEEK_CUR) == 5, "extend preserves file offset");
    CHECK(pread(fd, buffer, sizeof(buffer), 0) == sizeof(buffer) &&
          memcmp(buffer, "pat\0\0", sizeof(buffer)) == 0, "verify zero-filled extension");
    CHECK(ftruncate(fd, 0) == 0, "truncate to zero");
    CHECK(lseek(fd, 0, SEEK_CUR) == 5, "zero truncate preserves file offset");
    CHECK(pread(fd, buffer, sizeof(buffer), 0) == 0, "verify empty file");
    CHECK(pwrite(fd, "paths", sizeof(buffer), 0) == sizeof(buffer), "restore truncated file");
    pthread_t workers[2];
    for (int i = 0; i < 2; ++i)
        CHECK(pthread_create(&workers[i], NULL, shared_open_worker,
                             file_descriptor(fd)) == 0, "start shared open worker");
    for (int i = 0; i < 2; ++i) {
        void *result;
        CHECK(pthread_join(workers[i], &result) == 0 && result == NULL,
              "join shared open worker");
    }
    CHECK(close(fd) == 0, "close after write");
    CHECK(ftruncate(fd, 0) == -1 && errno == EBADF, "ftruncate closed fd");
    CHECK(pread(fd, buffer, 1, 0) == -1 && errno == EBADF, "pread closed fd");
    CHECK(pwrite(fd, buffer, 1, 0) == -1 && errno == EBADF, "pwrite closed fd");

    fd = open("./" FILE_NAME, O_WRONLY | O_APPEND);
    CHECK(fd >= 0, "open append fd");
    CHECK(pwrite(fd, "a", 1, 1) == 1, "pwrite ignores append mode");
    CHECK(lseek(fd, 0, SEEK_CUR) == 0, "append pwrite preserves offset");
    CHECK(pread(fd, buffer, 1, 0) == -1 && errno == EBADF, "pread write-only fd");
    CHECK(close(fd) == 0, "close append fd");

    fd = openat(AT_FDCWD, "../child/./" FILE_NAME, O_RDONLY);
    CHECK(fd >= 0, "openat relative to cwd");
    CHECK(fstat(fd, &other_stat) == 0, "fstat reopened file");
    CHECK(file_stat.st_dev == other_stat.st_dev && file_stat.st_ino == other_stat.st_ino,
          "fstat identity across opens");

    char native_name[] = "game:\\picolibc-path-xenia-dir\\child\\" FILE_NAME;
    ANSI_STRING name = { .Length = sizeof(native_name) - 1,
                         .MaximumLength = sizeof(native_name), .Buffer = native_name };
    X_OBJECT_ATTRIBUTES attrs = { .RootDirectory = NULL, .ObjectName = &name,
                                  .Attributes = 0 };
    X_IO_STATUS_BLOCK io_status;
    LARGE_INTEGER allocation_size = { .QuadPart = 0 };
    HANDLE native_handle;
    NTSTATUS status = NtCreateFile(&native_handle, FILE_READ_DATA | SYNCHRONIZE,
                                   &attrs, &io_status, &allocation_size,
                                   FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                                   FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    CHECK(NT_SUCCESS(status), "open native metadata handle");
    X_FILE_NETWORK_OPEN_INFORMATION info;
    status = NtQueryInformationFile(native_handle, &io_status, &info, sizeof(info),
                                    FileNetworkOpenInformation);
    CHECK(NT_SUCCESS(status), "query native metadata");
    X_FILE_INTERNAL_INFORMATION internal_info;
    status = NtQueryInformationFile(native_handle, &io_status, &internal_info,
                                    sizeof(internal_info), FileInternalInformation);
    CHECK(NT_SUCCESS(status), "query native file ID");
    CHECK(other_stat.st_ino == internal_info.IndexNumber, "fstat file ID");
    union {
        LARGE_INTEGER alignment;
        unsigned char bytes[PATH_MAX + 32];
    } volume_info;
    status = NtQueryVolumeInformationFile(native_handle, &io_status, volume_info.bytes,
                                          sizeof(volume_info.bytes), 1);
    CHECK(NT_SUCCESS(status), "query native volume");
    ULONG volume_serial;
    memcpy(&volume_serial, volume_info.bytes + 8, sizeof(volume_serial));
    CHECK(other_stat.st_dev == volume_serial, "fstat volume serial");
    CHECK(matches_filetime(other_stat.st_atim, info.LastAccessTime.QuadPart) &&
          matches_filetime(other_stat.st_mtim, info.LastWriteTime.QuadPart) &&
          matches_filetime(other_stat.st_ctim, info.ChangeTime.QuadPart),
          "fstat timestamps");
    CHECK(NT_SUCCESS(NtClose(native_handle)), "close native metadata handle");
    CHECK(ftruncate(fd, 0) == -1 && errno == EBADF, "ftruncate read-only fd");
    CHECK(pwrite(fd, buffer, 1, 0) == -1 && errno == EBADF, "pwrite read-only fd");
    CHECK(pread(fd, buffer, 1, 5) == 0, "append pwrite did not extend file");
    CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer), "read cwd-relative path");
    CHECK(memcmp(buffer, "paths", sizeof(buffer)) == 0, "verify cwd-relative path");
    CHECK(close(fd) == 0, "close cwd-relative path");

    int base_dirfd = open(POSIX_DIR, O_RDWR | O_DIRECTORY);
    CHECK(base_dirfd >= 0, "open absolute base directory");
    CHECK(fstatat(base_dirfd, "child/" FILE_NAME, &other_stat, 0) == 0 &&
          S_ISREG(other_stat.st_mode) && other_stat.st_ino == file_stat.st_ino &&
          other_stat.st_size == sizeof(buffer), "fstatat file relative to directory");
    fd = openat(base_dirfd, "child/../child/" FILE_NAME, O_RDONLY);
    CHECK(fd >= 0, "openat relative to absolute directory");
    CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer),
          "read path relative to absolute directory");
    CHECK(memcmp(buffer, "paths", sizeof(buffer)) == 0,
          "verify path relative to absolute directory");
    CHECK(close(fd) == 0, "close path relative to absolute directory");

    int child_dirfd = openat(base_dirfd, "child", O_RDWR | O_DIRECTORY);
    CHECK(child_dirfd >= 0, "open nested directory with openat");
    fd = openat(child_dirfd, FILE_NAME, O_RDONLY);
    CHECK(fd >= 0, "openat relative to nested directory");
    CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer),
          "read path relative to nested directory");
    CHECK(memcmp(buffer, "paths", sizeof(buffer)) == 0,
          "verify path relative to nested directory");
    CHECK(close(fd) == 0, "close path relative to nested directory");
    CHECK(close(child_dirfd) == 0, "close nested directory");
    CHECK(close(base_dirfd) == 0, "close absolute base directory");

    int relative_dirfd = open("..", O_RDWR | O_DIRECTORY);
    CHECK(relative_dirfd >= 0, "open cwd-relative base directory");
    fd = openat(relative_dirfd, "child/../child/" FILE_NAME, O_RDONLY);
    CHECK(fd >= 0, "openat relative to cwd-opened directory");
    CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer),
          "read path relative to cwd-opened directory");
    CHECK(memcmp(buffer, "paths", sizeof(buffer)) == 0,
          "verify path relative to cwd-opened directory");
    CHECK(close(fd) == 0, "close path relative to cwd-opened directory");

    int relative_childfd = openat(relative_dirfd, "child", O_RDWR | O_DIRECTORY);
    CHECK(relative_childfd >= 0, "open relative nested directory");
    fd = openat(relative_childfd, FILE_NAME, O_RDONLY);
    CHECK(fd >= 0, "openat relative to relative nested directory");
    CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer),
          "read path relative to relative nested directory");
    CHECK(memcmp(buffer, "paths", sizeof(buffer)) == 0,
          "verify path relative to relative nested directory");
    CHECK(close(fd) == 0, "close path relative to relative nested directory");
    CHECK(close(relative_childfd) == 0, "close relative nested directory");
    CHECK(close(relative_dirfd) == 0, "close cwd-relative base directory");

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

    CHECK(unlinkat(-1, POSIX_CHILD, AT_REMOVEDIR) == -1 && errno == ENOSYS,
          "unlinkat directory deletion is unsupported");
    CHECK(stat(POSIX_DOT_PATH, &other_stat) == 0 && S_ISREG(other_stat.st_mode),
          "unsupported directory deletion preserves contents");
    CHECK(unlink(POSIX_CHILD) == -1 && errno == EISDIR,
          "unlink rejects directory");
    CHECK(unlinkat(AT_FDCWD, FILE_NAME, 0x40000000) == -1 && errno == EINVAL,
          "unlinkat rejects unknown flags");
    CHECK(unlinkat(AT_FDCWD, NULL, 0) == -1 && errno == EFAULT,
          "unlinkat rejects null path");
    CHECK(unlinkat(AT_FDCWD, "", 0) == -1 && errno == ENOENT,
          "unlinkat rejects empty path");
    CHECK(unlinkat(-1, FILE_NAME, 0) == -1 && errno == EBADF,
          "unlinkat rejects invalid dirfd");
    fd = open(FILE_NAME, O_RDONLY);
    CHECK(fd >= 0, "open file for unlink tests");
    CHECK(unlinkat(fd, FILE_NAME, 0) == -1 && errno == ENOTDIR,
          "unlinkat rejects file as directory");
    CHECK(unlink("picolibc-PATH-xenia-test.bin") == -1 && errno == EBUSY,
          "unlink rejects open file irrespective of path case");
    CHECK(unlink("\\Device\\Harddisk0\\Partition1\\picolibc-path-xenia-dir\\child\\" FILE_NAME)
          == -1 && errno == EBUSY, "unlink rejects open file through device alias");
    CHECK(pread(fd, buffer, sizeof(buffer), 0) == sizeof(buffer) &&
          memcmp(buffer, "paths", sizeof(buffer)) == 0,
          "failed unlink preserves open file");
    CHECK(close(fd) == 0, "close file before unlink");
    CHECK(unlink(FILE_NAME "/") == -1 && errno == ENOTDIR,
          "unlink rejects trailing slash on file");
    CHECK(stat(FILE_NAME, &other_stat) == 0, "trailing slash preserves file");
    CHECK(unlinkat(-1, POSIX_DOT_PATH, 0) == 0,
          "unlinkat absolute path ignores dirfd");
    CHECK(stat(FILE_NAME, &other_stat) == -1 && errno == ENOENT,
          "unlinked file is absent");
    fd = open(FILE_NAME, O_CREAT | O_TRUNC | O_RDWR, 0);
    CHECK(fd >= 0 && close(fd) == 0, "recreate unlinked file");
    base_dirfd = open(POSIX_DIR, O_RDONLY | O_DIRECTORY);
    CHECK(base_dirfd >= 0, "open base directory for unlinkat");
    CHECK(unlinkat(base_dirfd, "child/" FILE_NAME, 0) == 0,
          "unlinkat relative to directory descriptor");
    CHECK(close(base_dirfd) == 0, "close unlinkat base directory");
    CHECK(unlink(FILE_NAME) == -1 && errno == ENOENT, "unlink missing file");
    fd = open(FILE_NAME, O_CREAT | O_TRUNC | O_RDWR, 0);
    CHECK(fd >= 0 && close(fd) == 0, "recreate file for unlink");
    CHECK(unlink(FILE_NAME) == 0, "unlink relative to current directory");
    CHECK(unlink(FILE_NAME) == -1 && errno == ENOENT, "verify unlink removal");

    const char *rename_source = "picolibc-rename-source.bin";
    const char *rename_target = "picolibc-rename-target.bin";
    CHECK(renameat(-1, NULL, -1, POSIX_CHILD) == -1 && errno == EFAULT,
          "renameat rejects null source");
    CHECK(renameat(-1, POSIX_CHILD, -1, NULL) == -1 && errno == EFAULT,
          "renameat rejects null destination");
    CHECK(renameat(AT_FDCWD, "", AT_FDCWD, rename_target) == -1 && errno == ENOENT,
          "renameat rejects empty source");
    CHECK(rename("missing-rename-file", "missing-rename-file") == -1 && errno == ENOENT,
          "rename missing path to itself");
    fd = open(rename_source, O_CREAT | O_EXCL | O_RDWR, 0);
    CHECK(fd >= 0, "create rename source");
    CHECK(write(fd, "source", 6) == 6, "write rename source");
    CHECK(rename(rename_source, rename_target) == -1 && errno == EBUSY,
          "rename rejects locally open source");
    CHECK(rename(rename_source, rename_source) == 0,
          "rename open file to itself");
    CHECK(close(fd) == 0, "close rename source");
    CHECK(rename(rename_source, "missing-directory/target") == -1 && errno == ENOENT,
          "rename to missing directory fails");
    CHECK(stat(rename_source, &other_stat) == 0, "failed rename preserves source");
    CHECK(rename(rename_source, rename_target) == 0, "rename file");
    CHECK(stat(rename_source, &other_stat) == -1 && errno == ENOENT,
          "rename removes old name");
    fd = open(rename_target, O_RDONLY);
    CHECK(fd >= 0 && read(fd, buffer, 5) == 5 && memcmp(buffer, "sourc", 5) == 0,
          "renamed file contents");
    CHECK(close(fd) == 0, "close renamed file");
    CHECK(rename("picolibc-rename-target.bin/", rename_source) == -1 && errno == ENOTDIR,
          "rename rejects trailing slash on file");
    fd = open(rename_source, O_CREAT | O_EXCL | O_RDWR, 0);
    CHECK(fd >= 0 && write(fd, "new", 3) == 3 && close(fd) == 0,
          "create replacement source");
    fd = open(rename_target, O_RDONLY);
    CHECK(fd >= 0 && rename(rename_source, rename_target) == -1 && errno == EBUSY,
          "rename rejects locally open destination");
    CHECK(close(fd) == 0, "close replacement destination");
    CHECK(rename(rename_source, rename_target) == 0, "replace existing file");
    fd = open(rename_target, O_RDONLY);
    CHECK(fd >= 0 && read(fd, buffer, 3) == 3 && memcmp(buffer, "new", 3) == 0,
          "replacement file contents");
    CHECK(close(fd) == 0, "close replacement file");
    base_dirfd = open(POSIX_DIR, O_RDONLY | O_DIRECTORY);
    child_dirfd = open(POSIX_CHILD, O_RDONLY | O_DIRECTORY);
    CHECK(base_dirfd >= 0 && child_dirfd >= 0, "open renameat directories");
    CHECK(renameat(child_dirfd, rename_target, base_dirfd, rename_source) == 0,
          "renameat across directories");
    CHECK(openat(child_dirfd, rename_target, O_RDONLY) == -1 && errno == ENOENT,
          "renameat removes old name");
    fd = openat(base_dirfd, rename_source, O_RDONLY);
    CHECK(fd >= 0 && read(fd, buffer, 3) == 3 && memcmp(buffer, "new", 3) == 0,
          "renameat moved file contents");
    CHECK(close(fd) == 0 && unlinkat(base_dirfd, rename_source, 0) == 0,
          "remove renameat test file");
    CHECK(renameat(-1, rename_target, base_dirfd, rename_source) == -1 && errno == EBADF,
          "renameat rejects invalid source dirfd");
    CHECK(close(child_dirfd) == 0 && close(base_dirfd) == 0,
          "close renameat directories");

    char native_source[] = "game:\\picolibc-native-rename-source.bin";
    char native_target[] = "game:\\picolibc-native-rename-target.bin";
    fd = open(native_source, O_CREAT | O_EXCL | O_RDWR, 0);
    CHECK(fd >= 0 && close(fd) == 0, "create native rename source");
    fd = open(native_target, O_CREAT | O_EXCL | O_RDWR, 0);
    CHECK(fd >= 0, "create native rename target");
    name = (ANSI_STRING){ sizeof(native_source) - 1, sizeof(native_source), native_source };
    attrs.ObjectName = &name;
    status = NtCreateFile(&native_handle, 0x00010000 | SYNCHRONIZE, &attrs,
                          &io_status, &allocation_size, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    CHECK(NT_SUCCESS(status), "open native rename source");
    X_FILE_RENAME_INFORMATION rename_info = {
        .ReplaceExisting = FALSE,
        .RootDirectory = 0,
        .FileName = { sizeof(native_target) - 1, sizeof(native_target), native_target },
    };
    status = NtSetInformationFile(native_handle, &io_status, &rename_info,
                                  sizeof(rename_info), FileRenameInformation);
    CHECK(status == (NTSTATUS)STATUS_OBJECT_NAME_COLLISION,
          "native rename honors no-replace");
    rename_info.ReplaceExisting = TRUE;
    status = NtSetInformationFile(native_handle, &io_status, &rename_info,
                                  sizeof(rename_info), FileRenameInformation);
    CHECK(status == (NTSTATUS)0xc0000043, "native rename rejects open target");
    CHECK(write(fd, "ok", 2) == 2, "failed native rename preserves target handle");
    CHECK(close(fd) == 0, "close native rename target");
    status = NtSetInformationFile(native_handle, &io_status, &rename_info,
                                  sizeof(rename_info), FileRenameInformation);
    CHECK(NT_SUCCESS(status) && NT_SUCCESS(NtClose(native_handle)),
          "native rename replaces closed target");
    CHECK(unlink(native_target) == 0, "remove native rename target");

    puts("PICO_PATH_PASS");
    return 0;
}
