/* Link with test-xbox-chmod.c and TEST_CHMOD_FALLBACK to exercise reopening. */
#define _GNU_SOURCE
#include <windef.h>
#include <xboxkrnl.h>

static ULONG NTAPI test_create(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                               PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
                               ULONG, ULONG);
static ULONG NTAPI test_set(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
static ULONG NTAPI test_query(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
static ULONG NTAPI test_query_volume(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
static ULONG NTAPI test_close(HANDLE);

#define NtCreateFile test_create
#define NtSetInformationFile test_set
#define NtQueryInformationFile test_query
#define NtQueryVolumeInformationFile test_query_volume
#define NtClose test_close
#include "../../libc/machine/powerpc/machine/io.c"
#undef NtCreateFile
#undef NtSetInformationFile
#undef NtQueryInformationFile
#undef NtQueryVolumeInformationFile
#undef NtClose

static HANDLE attribute_handle;
int test_chmod_identity_mismatch;
int test_chmod_volume_mismatch;

static ULONG NTAPI
test_create(PHANDLE out, ACCESS_MASK access, POBJECT_ATTRIBUTES attrs,
            PIO_STATUS_BLOCK status, PLARGE_INTEGER size, ULONG file_attrs,
            ULONG share, ULONG disposition, ULONG options)
{
    ULONG result = NtCreateFile(out, access, attrs, status, size, file_attrs,
                                share, disposition, options);
    if (NT_SUCCESS(result) && (access & 0x100))
        attribute_handle = *out;
    return result;
}

static ULONG NTAPI
test_set(HANDLE handle, PIO_STATUS_BLOCK status, PVOID info, ULONG length,
         ULONG info_class)
{
    if (info_class == FileBasicInformation && handle != attribute_handle)
        return STATUS_ACCESS_DENIED;
    return NtSetInformationFile(handle, status, info, length, info_class);
}

static ULONG NTAPI
test_query(HANDLE handle, PIO_STATUS_BLOCK status, PVOID info, ULONG length,
           ULONG info_class)
{
    ULONG result = NtQueryInformationFile(handle, status, info, length, info_class);
    if (NT_SUCCESS(result) && handle == attribute_handle &&
        info_class == FileInternalInformation && test_chmod_identity_mismatch)
        ((X_FILE_INTERNAL_INFORMATION *)info)->IndexNumber ^= 1;
    return result;
}

static ULONG NTAPI
test_query_volume(HANDLE handle, PIO_STATUS_BLOCK status, PVOID info,
                  ULONG length, ULONG info_class)
{
    ULONG result = NtQueryVolumeInformationFile(handle, status, info, length, info_class);
    if (NT_SUCCESS(result) && handle == attribute_handle && test_chmod_volume_mismatch) {
        struct volume_info {
            LARGE_INTEGER creation_time;
            ULONG serial_number;
        };
        ((struct volume_info *)info)->serial_number ^= 1;
    }
    return result;
}

static ULONG NTAPI
test_close(HANDLE handle)
{
    ULONG result = NtClose(handle);
    if (handle == attribute_handle)
        attribute_handle = NULL;
    return result;
}
