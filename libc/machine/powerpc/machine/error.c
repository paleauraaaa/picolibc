#include "error.h"

#include <errno.h>

int nt_status_to_errno(NTSTATUS status) {
    switch ((uint32_t)status) {
    case STATUS_INVALID_HANDLE:
        return EBADF;
    case STATUS_INVALID_PARAMETER:
    case STATUS_OBJECT_NAME_INVALID:
    case STATUS_INVALID_PAGE_PROTECTION:
        return EINVAL;
    case STATUS_NO_SUCH_FILE:
    case STATUS_OBJECT_NAME_NOT_FOUND:
    case STATUS_OBJECT_PATH_NOT_FOUND:
        return ENOENT;
    case STATUS_NO_MEMORY:
    case STATUS_INSUFFICIENT_RESOURCES:
        return ENOMEM;
    case STATUS_ACCESS_DENIED:
        return EACCES;
    case STATUS_OBJECT_NAME_COLLISION:
        return EEXIST;
    case STATUS_DISK_FULL:
        return ENOSPC;
    case STATUS_FILE_IS_A_DIRECTORY:
        return EISDIR;
    case STATUS_NOT_A_DIRECTORY:
        return ENOTDIR;
    default:
        return EIO;
    }
}
