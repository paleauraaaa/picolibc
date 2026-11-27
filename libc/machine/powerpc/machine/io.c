#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int openat(int dirfd, const char* path, int flags, ...) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }

    char cwd_path[PATH_MAX + 1];
    const char *path_to_open = path;
    bool absolute_path = is_absolute_path(path);
    if (!absolute_path && dirfd == AT_FDCWD) {
        pthread_mutex_lock(&s_cwd_mutex);
        if (s_cwd_fd == -1 ||
            append_path(s_cwd, path, cwd_path, sizeof(cwd_path), NULL) == -1) {
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

    char native_path[PATH_MAX + 1];
    size_t len;
    if (translate_path(path_to_open, native_path, sizeof(native_path), &len) == -1)
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
    for (int i = 0; i < MAX_FD - 3; i++) {
        if (fd == -1 && s_fds[i].file == NULL)
            fd = i;
        if (s_files[i].handle == NULL) {
            if (free_file == NULL)
                free_file = &s_files[i];
        } else if (!((flags & O_CREAT) && (flags & O_EXCL)) &&
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
            return -1;
        }
        result.st_mode = S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH;
        *buf = result;
        return 0;
    }

    file_descriptor_t *f = file_descriptor(fd);
    if (f == NULL || f->handle == NULL) {
        errno = EBADF;
        return -1;
    }
    if (buf == NULL) {
        errno = EFAULT;
        return -1;
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
    *buf = result;
    ret = 0;

done:
    pthread_mutex_unlock(&s_fds_mutex);
    return ret;
}

int stat(const char *path, struct stat *buf) {
    int fd = open(path, O_RDONLY);
    if (fd < 0 && errno == EISDIR)
        fd = open(path, O_RDONLY | O_DIRECTORY);
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

int lstat(const char *path, struct stat *buf) {
    (void) path;
    (void) buf;
    errno = ENOSYS;
    return -1;
}
