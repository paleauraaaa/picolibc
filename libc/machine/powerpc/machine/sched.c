#include <errno.h>
#include <sched.h>

#include <xboxkrnl.h>

#include "error.h"

int sched_yield(void) {
    NTSTATUS status = NtYieldExecution();
    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return -1;
    }
    return 0;
}