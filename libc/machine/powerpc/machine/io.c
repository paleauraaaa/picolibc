#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

#include <windef.h>
#include <xboxkrnl.h>

#include "error.h"

#ifndef MAX_FD
#define MAX_FD 1024
#endif

typedef struct file_descriptor {
    HANDLE          handle;
    ACCESS_MASK     access;
    off_t           off;
} file_descriptor;

static file_descriptor s_fds[MAX_FD] = { { NULL, 0, 0 } };

int open(const char* path, int flags, ...) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }

    int fd = -1;
    for (int i = 0; i < MAX_FD; i++) {
        if (s_fds[i].handle == NULL) {
            fd = i;
            break;
        }
    }
    if (fd == -1) {
        errno = EMFILE;
        return -1;
    }

    PHANDLE pHandle = &s_fds[fd].handle;

    size_t len = strlen(path);
    if (len == 0) {
        errno = ENOENT;
        return -1;
    }
    if (len + 1 > USHORT_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    ANSI_STRING ObjectName = {
        .Length = (USHORT)len,
        .MaximumLength = (USHORT)(len + 1),
        .Buffer = (CHAR*)path
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
        errno = nt_status_to_errno(status);
        return -1;
    }
    
    s_fds[fd].off = 0;
    s_fds[fd].access = DesiredAccess;
    return fd + 3;
}

int close (int fd) {
    fd -= 3;
    if (fd < 0 || fd >= MAX_FD || s_fds[fd].handle == NULL) {
        errno = EBADF;
        return -1;
    }

    NTSTATUS status = NtClose(s_fds[fd].handle);
    s_fds[fd].handle = NULL;
    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return -1;
    }

    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    fd -= 3;
    if (fd < 0 || fd >= MAX_FD || s_fds[fd].handle == NULL) {
        errno = EBADF;
        return -1;
    }

    uint64_t file_size = 0;
    X_IO_STATUS_BLOCK IoStatusBlock;
    X_FILE_NETWORK_OPEN_INFORMATION Info;
    NTSTATUS status = NtQueryInformationFile(s_fds[fd].handle, &IoStatusBlock, &Info, sizeof(Info), FileNetworkOpenInformation);
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
    if (fd < 0 || fd >= MAX_FD || s_fds[fd].handle == NULL) {
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
    if (fd < 0 || fd >= MAX_FD || s_fds[fd].handle == NULL) {
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
