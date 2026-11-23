#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    off_t           off;
    char           *path;
} file_descriptor;

static file_descriptor s_fds[MAX_FD - 3] = { { NULL, 0, 0, NULL } };

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
        if (dirfd < 3 || dirfd >= MAX_FD) {
            errno = EBADF;
            return -1;
        }
        int root_index = dirfd - 3;
        if (root_index >= MAX_FD - 3 ||
            s_fds[root_index].handle == NULL) {
            errno = EBADF;
            return -1;
        }
        if (s_fds[root_index].path == NULL) {
            errno = ENOTDIR;
            return -1;
        }
        if (append_path(s_fds[root_index].path, path, cwd_path,
                        sizeof(cwd_path), NULL) == -1)
            return -1;
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

    int fd = -1;
    for (int i = 0; i < MAX_FD - 3; i++) {
        if (s_fds[i].handle == NULL) {
            fd = i;
            break;
        }
    }
    if (fd == -1) {
        errno = EMFILE;
        return -1;
    }

    char native_path[PATH_MAX + 1];
    size_t len;
    if (translate_path(path_to_open, native_path, sizeof(native_path), &len) == -1)
        return -1;

    PHANDLE pHandle = &s_fds[fd].handle;

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
    ULONG ShareAccess = FILE_SHARE_READ;
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

    char *directory_path = NULL;
    if (flags & O_DIRECTORY) {
        size_t directory_path_length = strlen(path_to_open);
        directory_path = malloc(directory_path_length + 1);
        if (directory_path == NULL) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(directory_path, path_to_open, directory_path_length + 1);
    }

    NTSTATUS status = NtCreateFile(
        pHandle,
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
        free(directory_path);
        errno = nt_status_to_errno(status);
        return -1;
    }
    
    s_fds[fd].off = 0;
    s_fds[fd].access = DesiredAccess;
    s_fds[fd].path = directory_path;
    return fd + 3;
}

int close (int fd) {
    fd -= 3;
    if (fd < 0 || fd >= MAX_FD - 3 || s_fds[fd].handle == NULL) {
        errno = EBADF;
        return -1;
    }

    NTSTATUS status = NtClose(s_fds[fd].handle);
    s_fds[fd].handle = NULL;
    free(s_fds[fd].path);
    s_fds[fd].path = NULL;
    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return -1;
    }

    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    fd -= 3;
    if (fd < 0 || fd >= MAX_FD - 3 || s_fds[fd].handle == NULL) {
        errno = EBADF;
        return -1;
    }

    uint64_t file_size = 0;
    X_IO_STATUS_BLOCK IoStatusBlock;
    X_FILE_NETWORK_OPEN_INFORMATION Info;
    NTSTATUS status = NtQueryInformationFile(
        s_fds[fd].handle, &IoStatusBlock,
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
            new_off = s_fds[fd].off + offset;
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

    s_fds[fd].off = new_off;
    return new_off;
}

ssize_t read(int fd, void *buf, size_t count) {
    fd -= 3;
    if (fd < 0 || fd >= MAX_FD - 3 || s_fds[fd].handle == NULL) {
        errno = EBADF;
        return -1;
    }

    if (!(s_fds[fd].access & FILE_READ_DATA)) {
        errno = EBADF;
        return -1;
    }

    LARGE_INTEGER off = { .QuadPart = s_fds[fd].off };
    X_IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS status = NtReadFile(
        s_fds[fd].handle,
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
    s_fds[fd].off += bytes_read;
    return bytes_read;
}

ssize_t write(int fd, const void *buf, size_t count) {
    // TODO: This could definitely be made more efficient.
    // But it works for now.
    if (fd == STDOUT_FILENO) {
        return fwrite(buf, 1, count, stdout);
    } 
    
    if (fd == STDERR_FILENO) {
        return fwrite(buf, 1, count, stderr);
    }

    fd -= 3;
    if (fd < 0 || fd >= MAX_FD - 3 || s_fds[fd].handle == NULL) {
        errno = EBADF;
        return -1;
    }

    if (!(s_fds[fd].access & FILE_WRITE_DATA)) {
        errno = EBADF;
        return -1;
    }

    if (s_fds[fd].access & FILE_APPEND_DATA) {
        off_t off = lseek(fd + 3, 0, SEEK_END);
        if (off < 0)
            return -1;

        s_fds[fd].off = off;
    }

    X_IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER off = { .QuadPart = s_fds[fd].off };
    NTSTATUS status = NtWriteFile(
        s_fds[fd].handle,
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
    s_fds[fd].off += bytes_written;
    return bytes_written;
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
