/* Standalone Xbox guest test; link with the Xbox CRT and kernel/XAM imports. */
#define _GNU_SOURCE
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
    CHECK(close(dirfd) == 0, "close POSIX directory");
    CHECK(stat(POSIX_DIR, &file_stat) == 0 && S_ISDIR(file_stat.st_mode),
          "stat directory");
    errno = 0;
    CHECK(lstat(POSIX_DIR, &file_stat) == -1 && errno == ENOSYS,
          "lstat is unsupported");
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
    CHECK(fstatat(AT_FDCWD, "", &other_stat, AT_EMPTY_PATH) == 0 &&
          S_ISDIR(other_stat.st_mode), "fstatat empty path for current directory");
    char short_cwd[2];
    CHECK(getcwd(short_cwd, sizeof(short_cwd)) == NULL && errno == ERANGE,
          "getcwd rejects undersized buffer");

    int fd = open("./" FILE_NAME, O_CREAT | O_TRUNC | O_RDWR, 0);
    CHECK(fd >= 0, "open relative path");
    errno = 0;
    CHECK(fstat(fd, NULL) == -1 && errno == EFAULT, "fstat null buffer");
    CHECK(fstat(fd, &file_stat) == 0 && file_stat.st_size == 0, "fstat empty file");
    CHECK(write(fd, "paths", sizeof(buffer)) == sizeof(buffer), "write");
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

    puts("PICO_PATH_PASS");
    return 0;
}
