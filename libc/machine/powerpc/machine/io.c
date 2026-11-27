#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <sys/stat.h>
#include <unistd.h>

#include <windef.h>
#include <xboxkrnl.h>

#include "error.h"

#ifndef MAX_FD
#define MAX_FD 1024
#endif

static char s_cwd[PATH_MAX + 1] = "";
static pthread_mutex_t s_cwd_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_cwd_fd = -1;

static bool
is_absolute_path(const char *path)
{
    const char *separator;
    const char *colon = strchr(path, ':');

    if (path[0] == '/' || path[0] == '\\')
        return true;
    if (colon == NULL || colon == path)
        return false;

    separator = strpbrk(path, "/\\");
    return (separator == NULL || colon < separator) &&
           (colon[1] == '/' || colon[1] == '\\');
}

static bool
is_path_separator(char ch)
{
    return ch == '/' || ch == '\\';
}

static int
normalize_path(char *path, size_t capacity, size_t *normalized_length)
{
    size_t path_length = strlen(path);
    size_t read_index = 0;
    size_t write_index = 0;
    size_t protected_length = 0;
    bool posix_absolute = path[0] == '/';
    bool rooted_path = path[0] == '\\';
    bool relative_path = !posix_absolute && !rooted_path;
    char separator = relative_path ? '/' : (posix_absolute ? '/' : '\\');

    if (path_length == 0) {
        errno = ENOENT;
        return -1;
    }
    if (path_length >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (posix_absolute) {
        path[write_index++] = '/';
        read_index = 1;
        protected_length = write_index;
    } else if (rooted_path) {
        path[write_index++] = '\\';
        read_index = 1;
        while (read_index < path_length &&
               is_path_separator(path[read_index]))
            read_index++;
        protected_length = write_index;
    } else {
        char *colon = strchr(path, ':');
        char *first_separator = strpbrk(path, "/\\");

        if (colon != NULL && colon != path &&
            (first_separator == NULL || colon < first_separator) &&
            is_path_separator(colon[1])) {
            size_t volume_length = colon - path;
            path[volume_length + 1] = '\\';
            write_index = volume_length + 2;
            read_index = write_index;
            protected_length = write_index;
            relative_path = 0;
            separator = '\\';
        }
    }

    while (read_index < path_length) {
        while (read_index < path_length &&
               is_path_separator(path[read_index]))
            read_index++;
        if (read_index == path_length)
            break;

        size_t component_start = read_index;
        while (read_index < path_length &&
               !is_path_separator(path[read_index]))
            read_index++;
        size_t component_length = read_index - component_start;

        if (component_length == 1 && path[component_start] == '.')
            continue;

        if (component_length == 2 &&
            path[component_start] == '.' &&
            path[component_start + 1] == '.') {
            size_t last_component_start = write_index;
            while (last_component_start > 0 &&
                   !is_path_separator(path[last_component_start - 1]))
                last_component_start--;
            size_t last_component_length =
                write_index - last_component_start;
            int last_is_parent = relative_path &&
                last_component_length == 2 &&
                path[last_component_start] == '.' &&
                path[last_component_start + 1] == '.';

            if (write_index > protected_length && !last_is_parent) {
                while (write_index > protected_length &&
                       !is_path_separator(path[write_index - 1]))
                    write_index--;
                if (write_index > protected_length &&
                    is_path_separator(path[write_index - 1]))
                    write_index--;
            } else if (relative_path) {
                if (write_index != 0)
                    path[write_index++] = separator;
                path[write_index++] = '.';
                path[write_index++] = '.';
            }
            continue;
        }

        if (write_index != 0 &&
            !is_path_separator(path[write_index - 1]))
            path[write_index++] = separator;
        memmove(path + write_index, path + component_start, component_length);
        write_index += component_length;

        if (posix_absolute && protected_length == 1)
            protected_length = write_index;
    }

    if (relative_path && write_index == 0)
        path[write_index++] = '.';
    path[write_index] = '\0';
    if (normalized_length != NULL)
        *normalized_length = write_index;
    return 0;
}

static int
append_path(const char *base_path, const char *path, char *resolved_path,
            size_t capacity, size_t *resolved_length)
{
    size_t base_length = strlen(base_path);
    size_t path_length = strlen(path);
    size_t separator_length;

    if (base_length == 0 || path_length == 0) {
        errno = ENOENT;
        return -1;
    }

    if (base_length >= capacity || path_length >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }

    separator_length = base_path[base_length - 1] == '/' ||
                       base_path[base_length - 1] == '\\' ? 0 : 1;
    if (separator_length > capacity - base_length - 1 ||
        path_length > capacity - base_length - separator_length - 1) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(resolved_path, base_path, base_length);
    if (separator_length != 0)
        resolved_path[base_length] = '/';
    memcpy(resolved_path + base_length + separator_length, path, path_length);
    size_t length = base_length + separator_length + path_length;
    resolved_path[length] = '\0';
    return normalize_path(resolved_path, capacity, resolved_length);
}

char *
getcwd(char *buf, size_t size)
{
    bool allocated = buf == NULL;

    pthread_mutex_lock(&s_cwd_mutex);
    if (allocated) {
        if (size == 0)
            size = PATH_MAX + 1;
        buf = malloc(size);
        if (buf == NULL) {
            pthread_mutex_unlock(&s_cwd_mutex);
            return NULL;
        }
    }

    if (size == 0 || s_cwd_fd == -1 || s_cwd[0] == '\0') {
        int error = s_cwd_fd == -1 || s_cwd[0] == '\0' ? ENOENT : ERANGE;
        if (allocated)
            free(buf);
        pthread_mutex_unlock(&s_cwd_mutex);
        errno = error;
        return NULL;
    }

    size_t cwd_length = strlen(s_cwd);
    if (cwd_length >= size) {
        if (allocated)
            free(buf);
        pthread_mutex_unlock(&s_cwd_mutex);
        errno = ERANGE;
        return NULL;
    }

    memcpy(buf, s_cwd, cwd_length + 1);
    pthread_mutex_unlock(&s_cwd_mutex);
    return buf;
}

int chdir(const char* path) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }

    pthread_mutex_lock(&s_cwd_mutex);
    char new_cwd_path[PATH_MAX + 1];
    size_t len;
    if (is_absolute_path(path)) {
        len = strlen(path);
        if (len >= PATH_MAX) {
            pthread_mutex_unlock(&s_cwd_mutex);
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(new_cwd_path, path, len + 1);
        if (normalize_path(new_cwd_path, sizeof(new_cwd_path), &len) == -1) {
            pthread_mutex_unlock(&s_cwd_mutex);
            return -1;
        }
    } else {
        if (s_cwd_fd == -1 ||
            append_path(s_cwd, path, new_cwd_path, sizeof(new_cwd_path),
                        &len) == -1) {
            pthread_mutex_unlock(&s_cwd_mutex);
            return -1;
        }
        if (len >= PATH_MAX) {
            pthread_mutex_unlock(&s_cwd_mutex);
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    int new_cwd = open(new_cwd_path, O_RDWR | O_DIRECTORY);
    if (new_cwd == -1) {
        pthread_mutex_unlock(&s_cwd_mutex);
        return -1;
    }
    if (s_cwd_fd != -1) {
        if (close(s_cwd_fd) == -1) {
            pthread_mutex_unlock(&s_cwd_mutex);
            return -1;
        }
        s_cwd_fd = -1;
    }
    s_cwd_fd = new_cwd;
    memcpy(s_cwd, new_cwd_path, len + 1);
    pthread_mutex_unlock(&s_cwd_mutex);
    return 0;
}

typedef struct file_descriptor {
    HANDLE          handle;
    ACCESS_MASK     access;
    char           *path;
    bool            is_dir;
    int             rcount;
} file_descriptor_t;

typedef struct {
    file_descriptor_t *file;
    off_t off;
} fd_slot_t;

static file_descriptor_t s_files[MAX_FD - 3];
static fd_slot_t s_fds[MAX_FD - 3];
static pthread_mutex_t s_fds_mutex = PTHREAD_MUTEX_INITIALIZER;

static fd_slot_t *
file_slot(int fd)
{
    if (fd < 3 || fd >= MAX_FD || s_fds[fd - 3].file == NULL)
        return NULL;
    return &s_fds[fd - 3];
}

file_descriptor_t* file_descriptor(int fd) {
    fd_slot_t *slot = file_slot(fd);
    return slot != NULL ? slot->file : NULL;
}

static int
set_file_length(file_descriptor_t *f, off_t length)
{
    X_FILE_END_OF_FILE_INFORMATION info = { .EndOfFile.QuadPart = length };
    X_IO_STATUS_BLOCK io_status;
    NTSTATUS status = NtSetInformationFile(f->handle, &io_status,
                                           &info, sizeof(info),
                                           FileEndOfFileInformation);
    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return -1;
    }
    return 0;
}

int open(const char* path, int flags, ...) {
    return openat(AT_FDCWD, path, flags);
}

static int
translate_path(const char *path, char *native_path, size_t capacity,
               size_t *translated_length)
{
    size_t path_length = strlen(path);
    size_t output_length = path_length;
    const char *tail = path;
    size_t volume_length = 0;
    bool posix_absolute = path[0] == '/';

    if (path_length == 0) {
        errno = ENOENT;
        return -1;
    }
    if (path_length > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (posix_absolute) {
        /* The first POSIX path component names the Xbox volume. */
        const char *volume = path + 1;
        const char *separator = strchr(volume, '/');

        volume_length = separator ? (size_t)(separator - volume) : path_length - 1;
        if (volume_length == 0 || memchr(volume, ':', volume_length) != NULL) {
            errno = EINVAL;
            return -1;
        }

        tail = separator ? separator + 1 : path + path_length;
        output_length = volume_length + 2 + strlen(tail);
    }

    if (output_length >= capacity || output_length >= USHORT_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    size_t output_index = 0;
    if (posix_absolute) {
        memcpy(native_path, path + 1, volume_length);
        output_index = volume_length;
        native_path[output_index++] = ':';
        native_path[output_index++] = '\\';
    }

    while (*tail != '\0') {
        char ch = *tail++;
        native_path[output_index++] = ch == '/' ? '\\' : ch;
    }
    native_path[output_index] = '\0';
    *translated_length = output_index;
    return 0;
}

static int
resolve_at_path(int dirfd, const char *path, char *native_path,
                size_t capacity, size_t *native_length)
{
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }

    char cwd_path[PATH_MAX + 1];
    const char *path_to_open = path;
    bool absolute_path = is_absolute_path(path);
    if (!absolute_path && dirfd == AT_FDCWD) {
        pthread_mutex_lock(&s_cwd_mutex);
        if (s_cwd_fd == -1) {
            pthread_mutex_unlock(&s_cwd_mutex);
            errno = ENOENT;
            return -1;
        }
        if (append_path(s_cwd, path, cwd_path, sizeof(cwd_path), NULL) == -1) {
            pthread_mutex_unlock(&s_cwd_mutex);
            return -1;
        }
        pthread_mutex_unlock(&s_cwd_mutex);
        path_to_open = cwd_path;
    } else if (!absolute_path) {
        pthread_mutex_lock(&s_fds_mutex);
        file_descriptor_t *root = file_descriptor(dirfd);
        if (root == NULL) {
            pthread_mutex_unlock(&s_fds_mutex);
            errno = EBADF;
            return -1;
        }
        if (!root->is_dir) {
            pthread_mutex_unlock(&s_fds_mutex);
            errno = ENOTDIR;
            return -1;
        }
        if (append_path(root->path, path, cwd_path,
                        sizeof(cwd_path), NULL) == -1) {
            pthread_mutex_unlock(&s_fds_mutex);
            return -1;
        }
        pthread_mutex_unlock(&s_fds_mutex);
        path_to_open = cwd_path;
    } else {
        size_t path_length = strlen(path);
        if (path_length >= sizeof(cwd_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(cwd_path, path, path_length + 1);
        if (normalize_path(cwd_path, sizeof(cwd_path), NULL) == -1)
            return -1;
        path_to_open = cwd_path;
    }

    return translate_path(path_to_open, native_path, capacity, native_length);
}

int openat(int dirfd, const char* path, int flags, ...) {
    char native_path[PATH_MAX + 1];
    size_t len;
    if (resolve_at_path(dirfd, path, native_path, sizeof(native_path), &len) == -1)
        return -1;

    ANSI_STRING ObjectName = {
        .Length = (USHORT)len,
        .MaximumLength = (USHORT)(len + 1),
        .Buffer = (CHAR*)native_path
    };

    X_OBJECT_ATTRIBUTES ObjectAttributes = {
        .RootDirectory = NULL,
        .ObjectName = &ObjectName,
        .Attributes = 0
    };

    X_IO_STATUS_BLOCK IoStatusBlock = { 0 };
    LARGE_INTEGER AllocationSize = { .QuadPart = 0 };

    ACCESS_MASK DesiredAccess = 0;
    ULONG FileAttributes = FILE_ATTRIBUTE_NORMAL;
    ULONG ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    ULONG CreateDisposition = 0;
    ULONG CreateOptions = FILE_SYNCHRONOUS_IO_NONALERT;

    if (flags & O_RDWR)
        DesiredAccess = FILE_READ_DATA | FILE_WRITE_DATA;
    else if (flags & O_WRONLY)
        DesiredAccess = FILE_WRITE_DATA;
    else
        DesiredAccess = FILE_READ_DATA;

    if (flags & O_APPEND)
        DesiredAccess |= FILE_APPEND_DATA;

    DesiredAccess |= SYNCHRONIZE;

    if ((flags & O_CREAT) && (flags & O_EXCL))
        CreateDisposition = FILE_CREATE;
    else if ((flags & O_CREAT) && (flags & O_TRUNC))
        CreateDisposition = FILE_OVERWRITE_IF;
    else if (flags & O_CREAT)
        CreateDisposition = FILE_OPEN_IF;
    else if (flags & O_TRUNC)
        CreateDisposition = FILE_OVERWRITE;
    else
        CreateDisposition = FILE_OPEN;

    if ((CreateDisposition == FILE_OPEN_IF || 
         CreateDisposition == FILE_OVERWRITE || 
         CreateDisposition == FILE_OVERWRITE_IF) && 
        ((DesiredAccess & FILE_WRITE_DATA) == 0))
    {
        errno = EACCES;
        return -1;
    }

    if (flags & O_DIRECTORY) {
        if (CreateDisposition != FILE_CREATE && 
            CreateDisposition != FILE_OPEN && 
            CreateDisposition != FILE_OPEN_IF) 
        {
            errno = EINVAL;
            return -1;
        }
        CreateOptions |= FILE_DIRECTORY_FILE;
    } else {
        CreateOptions |= FILE_NON_DIRECTORY_FILE;
    }

    pthread_mutex_lock(&s_fds_mutex);
    int fd = -1;
    file_descriptor_t *free_file = NULL;
    file_descriptor_t *f = NULL;
    /* Directory enumeration state belongs to the native handle. */
    for (int i = 0; i < MAX_FD - 3; i++) {
        if (fd == -1 && s_fds[i].file == NULL)
            fd = i;
        if (s_files[i].handle == NULL) {
            if (free_file == NULL)
                free_file = &s_files[i];
        } else if (!(flags & O_DIRECTORY) &&
                   !((flags & O_CREAT) && (flags & O_EXCL)) &&
                   s_files[i].access == DesiredAccess &&
                   s_files[i].is_dir == ((flags & O_DIRECTORY) != 0) &&
                   strcmp(s_files[i].path, native_path) == 0) {
            f = &s_files[i];
        }
    }
    if (fd == -1) {
        pthread_mutex_unlock(&s_fds_mutex);
        errno = EMFILE;
        return -1;
    }
    if (f != NULL) {
        if ((flags & O_TRUNC) && set_file_length(f, 0) < 0) {
            pthread_mutex_unlock(&s_fds_mutex);
            return -1;
        }
        ++f->rcount;
        s_fds[fd].file = f;
        s_fds[fd].off = 0;
        pthread_mutex_unlock(&s_fds_mutex);
        return fd + 3;
    }
    if (free_file == NULL) {
        pthread_mutex_unlock(&s_fds_mutex);
        errno = EMFILE;
        return -1;
    }

    char *saved_path = malloc(len + 1);
    if (saved_path == NULL) {
        pthread_mutex_unlock(&s_fds_mutex);
        errno = ENOMEM;
        return -1;
    }
    memcpy(saved_path, native_path, len + 1);
    f = free_file;

    NTSTATUS status = NtCreateFile(
        &f->handle,
        DesiredAccess,
        &ObjectAttributes,
        &IoStatusBlock,
        &AllocationSize,
        FileAttributes,
        ShareAccess,
        CreateDisposition,
        CreateOptions
    );
    if (!NT_SUCCESS(status)) {
        f->handle = NULL;
        free(saved_path);
        pthread_mutex_unlock(&s_fds_mutex);
        errno = nt_status_to_errno(status);
        return -1;
    }
    
    f->access = DesiredAccess;
    f->path = saved_path;
    f->is_dir = (flags & O_DIRECTORY) != 0;
    f->rcount = 1;
    s_fds[fd].file = f;
    s_fds[fd].off = 0;
    pthread_mutex_unlock(&s_fds_mutex);
    return fd + 3;
}

int close (int fd) {
    pthread_mutex_lock(&s_fds_mutex);
    fd_slot_t *slot = file_slot(fd);
    if (slot == NULL) {
        pthread_mutex_unlock(&s_fds_mutex);
        errno = EBADF;
        return -1;
    }

    file_descriptor_t *f = slot->file;
    slot->file = NULL;
    if (--f->rcount != 0) {
        pthread_mutex_unlock(&s_fds_mutex);
        return 0;
    }
    NTSTATUS status = NtClose(f->handle);
    f->handle = NULL;
    free(f->path);
    f->path = NULL;
    pthread_mutex_unlock(&s_fds_mutex);
    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return -1;
    }

    return 0;
}

static int ftruncate_locked(int fd, off_t length) {
    file_descriptor_t *f = file_descriptor(fd);
    if (f == NULL || !(f->access & FILE_WRITE_DATA)) {
        errno = EBADF;
        return -1;
    }
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }

    return set_file_length(f, length);
}

int ftruncate(int fd, off_t length) {
    pthread_mutex_lock(&s_fds_mutex);
    int result = ftruncate_locked(fd, length);
    pthread_mutex_unlock(&s_fds_mutex);
    return result;
}

static off_t lseek_locked(int fd, off_t offset, int whence) {
    fd_slot_t *slot = file_slot(fd);
    if (slot == NULL) {
        errno = EBADF;
        return -1;
    }
    file_descriptor_t *f = slot->file;

    uint64_t file_size = 0;
    X_IO_STATUS_BLOCK IoStatusBlock;
    X_FILE_NETWORK_OPEN_INFORMATION Info;
    NTSTATUS status = NtQueryInformationFile(
        f->handle, &IoStatusBlock,
        &Info, sizeof(Info),
        FileNetworkOpenInformation);

    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return -1;
    }

    file_size = Info.EndOfFile.QuadPart;
    off_t new_off;
    switch (whence) {
        case SEEK_SET:
            new_off = offset;
            break;
        case SEEK_CUR:
            new_off = slot->off + offset;
            break;
        case SEEK_END:
            new_off = file_size + offset;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    if (new_off < 0) {
        errno = EINVAL;
        return -1;
    }

    slot->off = new_off;
    return new_off;
}

off_t lseek(int fd, off_t offset, int whence) {
    pthread_mutex_lock(&s_fds_mutex);
    off_t result = lseek_locked(fd, offset, whence);
    pthread_mutex_unlock(&s_fds_mutex);
    return result;
}

static ssize_t read_locked(int fd, void *buf, size_t count) {
    fd_slot_t *slot = file_slot(fd);
    if (slot == NULL) {
        errno = EBADF;
        return -1;
    }
    file_descriptor_t *f = slot->file;

    if (!(f->access & FILE_READ_DATA)) {
        errno = EBADF;
        return -1;
    }

    LARGE_INTEGER off = { .QuadPart = slot->off };
    X_IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS status = NtReadFile(
        f->handle,
        NULL,
        NULL,
        NULL,
        &IoStatusBlock,
        buf,
        count,
        &off
    );
    if (!NT_SUCCESS(status)) {
        if ((uint32_t)status == STATUS_END_OF_FILE)
            return 0;

        errno = nt_status_to_errno(status);
        return -1;
    }

    uint64_t bytes_read = IoStatusBlock.Information;
    slot->off += bytes_read;
    return bytes_read;
}

ssize_t read(int fd, void *buf, size_t count) {
    pthread_mutex_lock(&s_fds_mutex);
    ssize_t result = read_locked(fd, buf, count);
    pthread_mutex_unlock(&s_fds_mutex);
    return result;
}

static ssize_t pread_locked(int fd, void *buf, size_t count, off_t offset) {
    file_descriptor_t *f = file_descriptor(fd);
    if (f == NULL || !(f->access & FILE_READ_DATA)) {
        errno = EBADF;
        return -1;
    }
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if (count > SSIZE_MAX)
        count = SSIZE_MAX;

    LARGE_INTEGER off = { .QuadPart = offset };
    X_IO_STATUS_BLOCK io_status;
    NTSTATUS status = NtReadFile(f->handle, NULL, NULL, NULL,
                                 &io_status, buf, count, &off);
    if (!NT_SUCCESS(status)) {
        if ((uint32_t)status == STATUS_END_OF_FILE)
            return 0;
        errno = nt_status_to_errno(status);
        return -1;
    }
    return io_status.Information;
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    pthread_mutex_lock(&s_fds_mutex);
    ssize_t result = pread_locked(fd, buf, count, offset);
    pthread_mutex_unlock(&s_fds_mutex);
    return result;
}

static ssize_t pwrite_locked(int fd, const void *buf, size_t count, off_t offset) {
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        errno = ESPIPE;
        return -1;
    }
    file_descriptor_t *f = file_descriptor(fd);
    if (f == NULL || !(f->access & FILE_WRITE_DATA)) {
        errno = EBADF;
        return -1;
    }
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if (count > SSIZE_MAX)
        count = SSIZE_MAX;

    LARGE_INTEGER off = { .QuadPart = offset };
    X_IO_STATUS_BLOCK io_status;
    NTSTATUS status = NtWriteFile(f->handle, NULL, NULL, NULL,
                                  &io_status, (PVOID)buf, count, &off);
    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return -1;
    }
    return io_status.Information;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    pthread_mutex_lock(&s_fds_mutex);
    ssize_t result = pwrite_locked(fd, buf, count, offset);
    pthread_mutex_unlock(&s_fds_mutex);
    return result;
}

static ssize_t write_locked(int fd, const void *buf, size_t count) {
    fd_slot_t *slot = file_slot(fd);
    if (slot == NULL) {
        errno = EBADF;
        return -1;
    }
    file_descriptor_t *f = slot->file;

    if (!(f->access & FILE_WRITE_DATA)) {
        errno = EBADF;
        return -1;
    }

    if (f->access & FILE_APPEND_DATA) {
        off_t off = lseek_locked(fd, 0, SEEK_END);
        if (off < 0)
            return -1;

        slot->off = off;
    }

    X_IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER off = { .QuadPart = slot->off };
    NTSTATUS status = NtWriteFile(
        f->handle,
        NULL,
        NULL,
        NULL,
        &IoStatusBlock,
        (PVOID)buf,
        count,
        &off
    );
    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return -1;
    }

    uint64_t bytes_written = IoStatusBlock.Information;
    slot->off += bytes_written;
    return bytes_written;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd == STDOUT_FILENO)
        return fwrite(buf, 1, count, stdout);
    if (fd == STDERR_FILENO)
        return fwrite(buf, 1, count, stderr);
    pthread_mutex_lock(&s_fds_mutex);
    ssize_t result = write_locked(fd, buf, count);
    pthread_mutex_unlock(&s_fds_mutex);
    return result;
}

int __stdio_flush(FILE *stream);
int __stdio_putc(char c, FILE *stream);
FILE __stdout = FDEV_SETUP_STREAM(__stdio_putc, NULL, __stdio_flush, _FDEV_SETUP_WRITE);
FILE __stderr = FDEV_SETUP_STREAM(__stdio_putc, NULL, __stdio_flush, _FDEV_SETUP_WRITE);
FILE *const stdout = &__stdout;
FILE *const stderr = &__stderr;

#define STDOUT_BUF_SIZE 4096
char   __stdout_buf[STDOUT_BUF_SIZE] = {0};
size_t __stdout_buf_pos              =  0;

int __stdio_flush(FILE *stream) {
    if (stream == stdout && __stdout_buf_pos != 0) {
        __stdout_buf[__stdout_buf_pos] = '\0';
        DbgPrint("%s", __stdout_buf);
        __stdout_buf_pos = 0;
        __stdout_buf[0] = '\0';
    }
    return 0;
}

int __stdio_putc(char c, FILE *stream) {
    if (stream == stderr) {
        if (c == '\0')
            DbgPrint("\\0");
        else
            DbgPrint("%c", (unsigned char)c);
        return 0;
    }

    if (stream == stdout) {
        if (c == '\0') {
            if (__stdout_buf_pos >= STDOUT_BUF_SIZE - 2)
                __stdio_flush(stream);
            __stdout_buf[__stdout_buf_pos++] = '\\';
            __stdout_buf[__stdout_buf_pos++] = '0';
            return 0;
        }

        if (__stdout_buf_pos >= STDOUT_BUF_SIZE - 1)
            __stdio_flush(stream);
        __stdout_buf[__stdout_buf_pos++] = c;

        if (c == '\n')
            return __stdio_flush(stream);
    }

    return 0;
}

static int
filetime_to_timespec(LONGLONG ticks, struct timespec *result)
{
    if (ticks < 0) {
        errno = EOVERFLOW;
        return -1;
    }
    if (ticks == 0) {
        result->tv_sec = 0;
        result->tv_nsec = 0;
    } else {
        result->tv_sec = ticks / 10000000LL - 11644473600LL;
        result->tv_nsec = (long)((ticks % 10000000LL) * 100);
    }
    return 0;
}

int fstat(int fd, struct stat *buf) {
    pthread_mutex_lock(&s_fds_mutex);

    enum { FileFsVolumeInformation = 1 };
    struct stat result = {0};
    X_IO_STATUS_BLOCK io_status;
    X_FILE_NETWORK_OPEN_INFORMATION info;
    X_FILE_INTERNAL_INFORMATION internal_info;
    struct {
        LARGE_INTEGER creation_time;
        ULONG serial_number;
        ULONG label_length;
        UCHAR supports_objects;
        char label[PATH_MAX + 1];
    } volume_info;
    NTSTATUS status;

    int ret = -1;
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        if (buf == NULL) {
            errno = EFAULT;
            goto done;
        }
        result.st_mode = S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH;
        *buf = result;
        ret = 0;
        goto done;
    }

    file_descriptor_t *f = file_descriptor(fd);
    if (f == NULL || f->handle == NULL) {
        errno = EBADF;
        goto done;
    }
    if (buf == NULL) {
        errno = EFAULT;
        goto done;
    }

    status = NtQueryInformationFile(f->handle, &io_status, &info, sizeof(info),
                                    FileNetworkOpenInformation);
    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        goto done;
    }
    status = NtQueryInformationFile(f->handle, &io_status, &internal_info,
                                    sizeof(internal_info), FileInternalInformation);
    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        goto done;
    }
    status = NtQueryVolumeInformationFile(f->handle, &io_status, &volume_info,
                                          sizeof(volume_info), FileFsVolumeInformation);
    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        goto done;
    }

    if (info.EndOfFile.QuadPart < 0 || info.AllocationSize.QuadPart < 0) {
        errno = EOVERFLOW;
        goto done;
    }
    if (filetime_to_timespec(info.LastAccessTime.QuadPart, &result.st_atim) < 0 ||
        filetime_to_timespec(info.LastWriteTime.QuadPart, &result.st_mtim) < 0 ||
        filetime_to_timespec(info.ChangeTime.QuadPart, &result.st_ctim) < 0)
    {
        errno = EOVERFLOW;
        goto done;
    }

    result.st_dev = volume_info.serial_number;
    result.st_ino = internal_info.IndexNumber;
    result.st_mode = (info.Attributes & FILE_ATTRIBUTE_DIRECTORY) ? S_IFDIR : S_IFREG;
    result.st_mode |= S_IRUSR | S_IRGRP | S_IROTH;
    if (!(info.Attributes & FILE_ATTRIBUTE_READONLY))
        result.st_mode |= S_IWUSR | S_IWGRP | S_IWOTH;
    if (info.Attributes & FILE_ATTRIBUTE_DIRECTORY)
        result.st_mode |= S_IXUSR | S_IXGRP | S_IXOTH;
    result.st_size = info.EndOfFile.QuadPart;
    result.st_blksize = 512;
    result.st_blocks = info.AllocationSize.QuadPart / 512 +
                       (info.AllocationSize.QuadPart % 512 != 0);
    result.st_uid = 1;
    result.st_gid = 1;
    *buf = result;
    ret = 0;

done:
    pthread_mutex_unlock(&s_fds_mutex);
    return ret;
}

int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        errno = EINVAL;
        return -1;
    }
    if (path == NULL || buf == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH)) {
            errno = ENOENT;
            return -1;
        }
        if (dirfd != AT_FDCWD)
            return fstat(dirfd, buf);

        pthread_mutex_lock(&s_cwd_mutex);
        int ret;
        if (s_cwd_fd == -1) {
            errno = ENOENT;
            ret = -1;
        } else {
            ret = fstat(s_cwd_fd, buf);
        }
        pthread_mutex_unlock(&s_cwd_mutex);
        return ret;
    }
    if (flags & AT_SYMLINK_NOFOLLOW) {
        errno = ENOSYS;
        return -1;
    }

    int fd = openat(dirfd, path, O_RDONLY);
    if (fd < 0 && errno == EISDIR)
        fd = openat(dirfd, path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return -1;
    int ret = fstat(fd, buf);
    if (ret < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return close(fd);
}

int stat(const char *path, struct stat *buf) {
    return fstatat(AT_FDCWD, path, buf, 0);
}

int lstat(const char *path, struct stat *buf) {
    (void) path;
    (void) buf;
    errno = ENOSYS;
    return -1;
}

int mkdirat(int dirfd, const char *path, mode_t mode) {
    (void)mode;
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (path[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    char native_path[PATH_MAX + 1];
    size_t len;
    if (resolve_at_path(dirfd, path, native_path, sizeof(native_path), &len) == -1)
        return -1;

    ANSI_STRING name = {
        .Length = (USHORT)len,
        .MaximumLength = (USHORT)(len + 1),
        .Buffer = native_path,
    };
    X_OBJECT_ATTRIBUTES attrs = {
        .RootDirectory = NULL,
        .ObjectName = &name,
        .Attributes = 0,
    };

    X_IO_STATUS_BLOCK io_status = {0};
    LARGE_INTEGER allocation_size = { .QuadPart = 0 };
    HANDLE handle;

    NTSTATUS status = NtCreateFile(
        &handle, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE, &attrs,
        &io_status, &allocation_size, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);

    if (NT_SUCCESS(status))
        status = NtClose(handle);

    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return -1;
    }
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    return mkdirat(AT_FDCWD, path, mode);
}

int unlinkat(int dirfd, const char *path, int flags) {
    if (flags & ~AT_REMOVEDIR) {
        errno = EINVAL;
        return -1;
    }
    if (flags & AT_REMOVEDIR) {
        errno = ENOSYS;
        return -1;
    }

    char native_path[PATH_MAX + 1];
    size_t len;
    if (resolve_at_path(dirfd, path, native_path, sizeof(native_path), &len) == -1)
        return -1;

    ANSI_STRING name = {
        .Length = (USHORT)len,
        .MaximumLength = (USHORT)(len + 1),
        .Buffer = native_path,
    };
    X_OBJECT_ATTRIBUTES attrs = {
        .RootDirectory = NULL,
        .ObjectName = &name,
        .Attributes = 0,
    };
    X_IO_STATUS_BLOCK io_status = {0};
    LARGE_INTEGER allocation_size = { .QuadPart = 0 };
    HANDLE handle;
    enum { DELETE_ACCESS = 0x00010000 };

    pthread_mutex_lock(&s_fds_mutex);
    for (int i = 0; i < MAX_FD - 3; i++) {
        if (s_files[i].handle != NULL &&
            strcasecmp(s_files[i].path, native_path) == 0) {
            int error = s_files[i].is_dir ? EISDIR : EBUSY;
            pthread_mutex_unlock(&s_fds_mutex);
            errno = error;
            return -1;
        }
    }

    NTSTATUS status = NtCreateFile(&handle, DELETE_ACCESS | SYNCHRONIZE,
                                   &attrs, &io_status, &allocation_size,
                                   FILE_ATTRIBUTE_NORMAL,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   FILE_OPEN,
                                   FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(status)) {
        pthread_mutex_unlock(&s_fds_mutex);
        errno = nt_status_to_errno(status);
        return -1;
    }

    size_t path_length = strlen(path);
    if (path_length != 0 && is_path_separator(path[path_length - 1])) {
        NTSTATUS close_status = NtClose(handle);
        pthread_mutex_unlock(&s_fds_mutex);
        errno = NT_SUCCESS(close_status) ? ENOTDIR : nt_status_to_errno(close_status);
        return -1;
    }

    X_FILE_INTERNAL_INFORMATION target_id;
    status = NtQueryInformationFile(handle, &io_status, &target_id,
                                    sizeof(target_id), FileInternalInformation);
    if (NT_SUCCESS(status)) {
        for (int i = 0; i < MAX_FD - 3; i++) {
            if (s_files[i].handle == NULL)
                continue;
            X_FILE_INTERNAL_INFORMATION open_id;
            status = NtQueryInformationFile(s_files[i].handle, &io_status,
                                            &open_id, sizeof(open_id),
                                            FileInternalInformation);
            if (!NT_SUCCESS(status) || target_id.IndexNumber == open_id.IndexNumber) {
                if (NT_SUCCESS(status))
                    errno = EBUSY;
                else
                    errno = nt_status_to_errno(status);
                int error = errno;
                NtClose(handle);
                pthread_mutex_unlock(&s_fds_mutex);
                errno = error;
                return -1;
            }
        }
    } else {
        int error = nt_status_to_errno(status);
        NtClose(handle);
        pthread_mutex_unlock(&s_fds_mutex);
        errno = error;
        return -1;
    }

    X_FILE_DISPOSITION_INFORMATION disposition = { .DeleteFile = TRUE };
    status = NtSetInformationFile(handle, &io_status, &disposition,
                                  sizeof(disposition), FileDispositionInformation);
    NTSTATUS close_status = NtClose(handle);
    if (NT_SUCCESS(status) && NT_SUCCESS(close_status)) {
        X_FILE_NETWORK_OPEN_INFORMATION info;
        NTSTATUS query_status = NtQueryFullAttributesFile(&attrs, &info);
        if (NT_SUCCESS(query_status)) {
            pthread_mutex_unlock(&s_fds_mutex);
            errno = EIO;
            return -1;
        } else if (query_status != (NTSTATUS)STATUS_NO_SUCH_FILE &&
                   query_status != (NTSTATUS)STATUS_OBJECT_NAME_NOT_FOUND &&
                   query_status != (NTSTATUS)STATUS_OBJECT_PATH_NOT_FOUND) {
            status = query_status;
        }
    }
    pthread_mutex_unlock(&s_fds_mutex);

    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return -1;
    }
    if (!NT_SUCCESS(close_status)) {
        errno = nt_status_to_errno(close_status);
        return -1;
    }
    return 0;
}

int unlink(const char *path) {
    return unlinkat(AT_FDCWD, path, 0);
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    char old_native[PATH_MAX + 1], new_native[PATH_MAX + 1];
    size_t old_len, new_len;
    if (resolve_at_path(olddirfd, oldpath, old_native, sizeof(old_native), &old_len) == -1 ||
        resolve_at_path(newdirfd, newpath, new_native, sizeof(new_native), &new_len) == -1)
        return -1;

    ANSI_STRING old_name = { (USHORT)old_len, (USHORT)(old_len + 1), old_native };
    ANSI_STRING new_name = { (USHORT)new_len, (USHORT)(new_len + 1), new_native };
    X_OBJECT_ATTRIBUTES old_attrs = { .RootDirectory = NULL, .ObjectName = &old_name, .Attributes = 0 };
    X_OBJECT_ATTRIBUTES new_attrs = { .RootDirectory = NULL, .ObjectName = &new_name, .Attributes = 0 };
    X_IO_STATUS_BLOCK io_status = {0};
    X_FILE_NETWORK_OPEN_INFORMATION old_info, new_info;
    X_FILE_INTERNAL_INFORMATION old_id, new_id;
    LARGE_INTEGER allocation_size = { .QuadPart = 0 };
    HANDLE source = NULL, target = NULL;
    NTSTATUS status;
    int error = 0;
    enum { DELETE_ACCESS = 0x00010000 };

    pthread_mutex_lock(&s_fds_mutex);
    status = NtQueryFullAttributesFile(&old_attrs, &old_info);
    if (!NT_SUCCESS(status)) {
        error = nt_status_to_errno(status);
        goto done;
    }

    status = NtQueryFullAttributesFile(&new_attrs, &new_info);
    bool target_exists = NT_SUCCESS(status);
    if (!target_exists && status != (NTSTATUS)STATUS_NO_SUCH_FILE &&
        status != (NTSTATUS)STATUS_OBJECT_NAME_NOT_FOUND &&
        status != (NTSTATUS)STATUS_OBJECT_PATH_NOT_FOUND) {
        error = nt_status_to_errno(status);
        goto done;
    }
    
    bool source_is_dir = (old_info.Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((is_path_separator(oldpath[strlen(oldpath) - 1]) && !source_is_dir) ||
        (is_path_separator(newpath[strlen(newpath) - 1]) &&
         (!target_exists || !(new_info.Attributes & FILE_ATTRIBUTE_DIRECTORY)))) {
        error = ENOTDIR;
        goto done;
    }

    if (strcasecmp(old_native, new_native) == 0)
        goto done;

    if (target_exists &&
        source_is_dir != ((new_info.Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
        error = source_is_dir ? ENOTDIR : EISDIR;
        goto done;
    }

    for (int i = 0; i < MAX_FD - 3; i++) {
        if (s_files[i].handle != NULL &&
            (source_is_dir || strcasecmp(s_files[i].path, old_native) == 0 ||
             strcasecmp(s_files[i].path, new_native) == 0)) {
            error = EBUSY;
            goto done;
        }
    }

    status = NtCreateFile(&source, DELETE_ACCESS | SYNCHRONIZE, &old_attrs,
                          &io_status, &allocation_size, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(status)) {
        error = nt_status_to_errno(status);
        goto done;
    }
    status = NtQueryInformationFile(source, &io_status, &old_id, sizeof(old_id),
                                    FileInternalInformation);
    if (!NT_SUCCESS(status)) {
        error = nt_status_to_errno(status);
        goto done;
    }
    if (target_exists) {
        status = NtCreateFile(&target, SYNCHRONIZE, &new_attrs, &io_status,
                              &allocation_size, FILE_ATTRIBUTE_NORMAL,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT);
        if (!NT_SUCCESS(status)) {
            error = nt_status_to_errno(status);
            goto done;
        }

        status = NtQueryInformationFile(target, &io_status, &new_id, sizeof(new_id),
                                        FileInternalInformation);
        if (!NT_SUCCESS(status)) {
            error = nt_status_to_errno(status);
            goto done;
        }

        if (old_id.IndexNumber == new_id.IndexNumber) {
            error = EBUSY;
            goto done;
        }
        
        status = NtClose(target);
        target = NULL;
        if (!NT_SUCCESS(status)) {
            error = nt_status_to_errno(status);
            goto done;
        }
    }

    for (int i = 0; i < MAX_FD - 3; i++) {
        if (s_files[i].handle == NULL)
            continue;
        X_FILE_INTERNAL_INFORMATION open_id;
        status = NtQueryInformationFile(s_files[i].handle, &io_status, &open_id,
                                        sizeof(open_id), FileInternalInformation);
        if (!NT_SUCCESS(status)) {
            error = nt_status_to_errno(status);
            goto done;
        }
        if (open_id.IndexNumber == old_id.IndexNumber ||
            (target_exists && open_id.IndexNumber == new_id.IndexNumber)) {
            error = EBUSY;
            goto done;
        }
    }
    X_FILE_RENAME_INFORMATION rename_info = {
        .ReplaceExisting = TRUE,
        .RootDirectory = 0,
        .FileName = new_name,
    };
    status = NtSetInformationFile(source, &io_status, &rename_info,
                                  sizeof(rename_info), FileRenameInformation);
    if (!NT_SUCCESS(status))
        error = nt_status_to_errno(status);

done:
    if (target != NULL) {
        status = NtClose(target);
        if (!NT_SUCCESS(status) && !error)
            error = nt_status_to_errno(status);
    }

    if (source != NULL) {
        status = NtClose(source);
        if (!NT_SUCCESS(status) && !error)
            error = nt_status_to_errno(status);
    }

    pthread_mutex_unlock(&s_fds_mutex);
    if (error) {
        errno = error;
        return -1;
    }

    return 0;
}

int rename(const char *oldpath, const char *newpath) {
    return renameat(AT_FDCWD, oldpath, AT_FDCWD, newpath);
}

DIR* fdopendir(int fd) {
    pthread_mutex_lock(&s_fds_mutex);
    file_descriptor_t *file = file_descriptor(fd);
    if (file == NULL || !(file->access & FILE_READ_DATA)) {
        pthread_mutex_unlock(&s_fds_mutex);
        errno = EBADF;
        return NULL;
    }

    if (!file->is_dir) {
        pthread_mutex_unlock(&s_fds_mutex);
        errno = ENOTDIR;
        return NULL;
    }

    DIR *dir = calloc(1, sizeof(*dir));
    if (dir == NULL) {
        pthread_mutex_unlock(&s_fds_mutex);
        errno = ENOMEM;
        return NULL;
    }

    dir->fd = fd;
    pthread_mutex_unlock(&s_fds_mutex);
    return dir;
}

DIR* opendir(const char *name) {
    int fd = open(name, O_RDONLY | O_DIRECTORY);
    if (fd == -1)
        return NULL;

    DIR *dir = fdopendir(fd);

    if (!dir) {
        int error = errno;
        close(fd);
        errno = error;
    }
    return dir;
}

int closedir(DIR *dir) {
    int result = close(dir->fd);
    int error = errno;
    free(dir);
    if (result == -1)
        errno = error;
    return result;
}

struct _FILE_DIRECTORY_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    CHAR FileName[];
};

_Static_assert(offsetof(FILE_DIRECTORY_INFORMATION, FileName) == 64,
               "unexpected Xbox directory information layout");

struct dirent *readdir(DIR *dir) {
    enum { READ_ATTRIBUTES = 0x80, REPARSE_POINT_ATTRIBUTE = 0x400 };
    const size_t header_size = offsetof(FILE_DIRECTORY_INFORMATION, FileName);
    FILE_DIRECTORY_INFORMATION info;
    X_IO_STATUS_BLOCK io_status = {0};

    pthread_mutex_lock(&s_fds_mutex);
    file_descriptor_t *file = file_descriptor(dir->fd);
    if (file == NULL || !file->is_dir || !(file->access & FILE_READ_DATA)) {
        pthread_mutex_unlock(&s_fds_mutex);
        errno = EBADF;
        return NULL;
    }

    if (dir->count == 0) {
        NTSTATUS status = NtQueryDirectoryFile(
            file->handle, NULL, NULL, NULL, &io_status,
            (FILE_DIRECTORY_INFORMATION *)(void *)dir->buf, sizeof(dir->buf),
            NULL, FALSE);
        if (!NT_SUCCESS(status)) {
            pthread_mutex_unlock(&s_fds_mutex);
            if ((uint32_t)status != 0x80000006 && /* STATUS_NO_MORE_FILES */
                (uint32_t)status != STATUS_NO_SUCH_FILE)
                errno = nt_status_to_errno(status);
            return NULL;
        }
        
        if (io_status.Information < header_size ||
            io_status.Information > sizeof(dir->buf)) {
            pthread_mutex_unlock(&s_fds_mutex);
            errno = EIO;
            return NULL;
        }
        dir->offset = 0;
        dir->count = io_status.Information;
    }

    size_t remaining = dir->count - dir->offset;
    if (remaining < header_size) {
        dir->count = 0;
        pthread_mutex_unlock(&s_fds_mutex);
        errno = EIO;
        return NULL;
    }

    memcpy(&info, dir->buf + dir->offset, header_size);
    if (info.FileNameLength == 0 ||
        info.FileNameLength > remaining - header_size ||
        (info.NextEntryOffset != 0 &&
         (info.NextEntryOffset < header_size + info.FileNameLength ||
          info.NextEntryOffset > remaining - header_size))) {
        dir->count = 0;
        pthread_mutex_unlock(&s_fds_mutex);
        errno = EIO;
        return NULL;
    }

    if (info.FileNameLength >= sizeof(dir->dirent.d_name)) {
        dir->count = 0;
        pthread_mutex_unlock(&s_fds_mutex);
        errno = ENAMETOOLONG;
        return NULL;
    }

    memcpy(dir->dirent.d_name, dir->buf + dir->offset + header_size,
           info.FileNameLength);
    dir->dirent.d_name[info.FileNameLength] = '\0';

    ANSI_STRING name = {
        .Length = (USHORT)info.FileNameLength,
        .MaximumLength = (USHORT)(info.FileNameLength + 1),
        .Buffer = dir->dirent.d_name,
    };

    X_OBJECT_ATTRIBUTES attrs = {
        .RootDirectory = file->handle,
        .ObjectName = &name,
        .Attributes = 0,
    };

    LARGE_INTEGER allocation_size = { .QuadPart = 0 };
    X_FILE_INTERNAL_INFORMATION internal_info;
    HANDLE entry_handle = NULL;
    NTSTATUS status = NtCreateFile(
        &entry_handle, READ_ATTRIBUTES | SYNCHRONIZE, &attrs, &io_status,
        &allocation_size, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT |
            ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ?
             FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE));

    if (NT_SUCCESS(status)) {
        status = NtQueryInformationFile(entry_handle, &io_status,
                                        &internal_info, sizeof(internal_info),
                                        FileInternalInformation);
        NTSTATUS close_status = NtClose(entry_handle);
        if (NT_SUCCESS(status))
            status = close_status;
    }

    if (!NT_SUCCESS(status)) {
        pthread_mutex_unlock(&s_fds_mutex);
        errno = nt_status_to_errno(status);
        return NULL;
    }

    dir->dirent.d_ino = internal_info.IndexNumber;
    if (info.FileAttributes & (FILE_ATTRIBUTE_DEVICE | REPARSE_POINT_ATTRIBUTE))
        dir->dirent.d_type = DT_UNKNOWN;
    else
        dir->dirent.d_type = (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ?
                             DT_DIR : DT_REG;

    if (info.NextEntryOffset == 0)
        dir->count = 0;
    else
        dir->offset += info.NextEntryOffset;

    pthread_mutex_unlock(&s_fds_mutex);
    return &dir->dirent;
}
