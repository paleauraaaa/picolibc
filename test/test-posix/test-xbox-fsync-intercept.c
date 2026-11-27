/* Link with test-xbox-fsync.c and TEST_FSYNC_INTERCEPT. */
#define _GNU_SOURCE
#include <windef.h>
#include <xboxkrnl.h>

static ULONG NTAPI test_flush(HANDLE, PIO_STATUS_BLOCK);
#define NtFlushBuffersFile test_flush
#include "../../libc/machine/powerpc/machine/io.c"
#undef NtFlushBuffersFile

int test_fsync_calls;
unsigned long test_fsync_status;

static ULONG NTAPI
test_flush(HANDLE handle, PIO_STATUS_BLOCK status)
{
    ++test_fsync_calls;
    if (test_fsync_status != 0)
        return test_fsync_status;
    return NtFlushBuffersFile(handle, status);
}
