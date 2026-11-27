#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <xboxkrnl.h>

#include "error.h"

#ifndef MMAP_MAX_ALLOC
#define MMAP_MAX_ALLOC 16384
#endif

#define MMAP_PAGE_SIZE          4096
#define MMAP_LARGE_PAGE_SIZE    65536
#define MMAP_4K_LIMIT           ((uintptr_t)0x40000000)
#define MMAP_64K_LIMIT          ((uintptr_t)0x80000000)

typedef struct mmap_info {
    void            *base_address;
    size_t           page_size;
    size_t           page_count;
    size_t           live_pages;
    unsigned char   *holes;
    unsigned char   *committed;
} mmap_info;

CRITICAL_SECTION mmap_lock;

static mmap_info s_mmap_infos[MMAP_MAX_ALLOC];

static size_t mmap_page_size(uintptr_t addr) {
    return addr >= MMAP_4K_LIMIT && addr < MMAP_64K_LIMIT ?
           MMAP_LARGE_PAGE_SIZE : MMAP_PAGE_SIZE;
}

static size_t mmap_page_count(size_t len, size_t page_size) {
    return len / page_size + (len % page_size != 0);
}

static int mmap_bitmap_test(const unsigned char *bitmap, size_t page) {
    return (bitmap[page / CHAR_BIT] & (1u << (page % CHAR_BIT))) != 0;
}

static void mmap_bitmap_set(unsigned char *bitmap, size_t page) {
    bitmap[page / CHAR_BIT] |= 1u << (page % CHAR_BIT);
}

static int mmap_page_is_live(const mmap_info *info, size_t page) {
    return !mmap_bitmap_test(info->holes, page);
}

/* The caller holds mmap_lock and has checked alignment and length. */
static mmap_info *mmap_find_range(uintptr_t start, size_t count, size_t *first)
{
    for (int i = 0; i < MMAP_MAX_ALLOC; i++) {
        mmap_info *info = &s_mmap_infos[i];
        uintptr_t base = (uintptr_t)info->base_address;
        if (!base || start < base)
            continue;
        size_t offset = (start - base) / info->page_size;
        if (offset < info->page_count && count <= info->page_count - offset) {
            *first = offset;
            return info;
        }
    }
    return NULL;
}

static void mmap_release_untracked(void *base) {
    SIZE_T size = 0;
    if (!NT_SUCCESS(NtFreeVirtualMemory(&base, &size, MEM_RELEASE, 0)))
        DbgPrint("mmap: failed to release unused reservation\n");
}

static int posix_prot_to_nt(int prot, ULONG *nt) {
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
        errno = EINVAL;
        return -1;
    }
    if ((prot & PROT_EXEC) || ((prot & PROT_WRITE) && !(prot & PROT_READ))) {
        errno = ENOTSUP;
        return -1;
    }
    *nt = prot == PROT_NONE ? PAGE_NOACCESS :
          (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    return 0;
}

static int mmap_check_free(uintptr_t start, size_t size, size_t page_size) {
    uintptr_t end = start + size;
    for (uintptr_t cursor = start; cursor < end;) {
        X_MEMORY_BASIC_INFORMATION info;
        NTSTATUS status = NtQueryVirtualMemory((void *)cursor, &info, 0);
        if (!NT_SUCCESS(status)) {
            errno = nt_status_to_errno(status);
            return -1;
        }
        if (info.State != MEM_FREE) {
            errno = EEXIST;
            return -1;
        }
        uintptr_t base = (uintptr_t)info.BaseAddress;
        if (base > cursor || info.RegionSize <= cursor - base) {
            errno = EIO;
            return -1;
        }
        size_t available = info.RegionSize - (cursor - base);
        if (available >= end - cursor)
            break;
        cursor += available;
        if (cursor % page_size != 0) {
            errno = EIO;
            return -1;
        }
    }
    return 0;
}

void* mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    if (len == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    ULONG Protect;
    if (posix_prot_to_nt(prot, &Protect) < 0)
        return MAP_FAILED;

    if (flags & MAP_SHARED) {
        errno = ENOTSUP;
        return MAP_FAILED;
    }

    if (!(flags & MAP_PRIVATE)) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    if (!(flags & MAP_ANONYMOUS)) {
        errno = ENODEV;
        return MAP_FAILED;
    }

    if (fd != -1 || offset != 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    bool fixed = (flags & MAP_FIXED) != 0;
    bool large_pages = (flags & MAP_LARGE_PAGES) != 0;
    uintptr_t hint = (uintptr_t)addr;
    if (large_pages && hint != 0 && (hint < MMAP_4K_LIMIT || hint >= MMAP_64K_LIMIT)) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    size_t page_size = large_pages ? MMAP_LARGE_PAGE_SIZE : mmap_page_size(hint);
    uintptr_t range_start = page_size == MMAP_LARGE_PAGE_SIZE ? MMAP_4K_LIMIT : 0;
    uintptr_t range_limit = page_size == MMAP_LARGE_PAGE_SIZE ? MMAP_64K_LIMIT : MMAP_4K_LIMIT;
    if (fixed) {
        if (hint == 0 || hint % page_size != 0) {
            errno = EINVAL;
            return MAP_FAILED;
        }
        if (hint >= MMAP_64K_LIMIT) {
            errno = ENOTSUP;
            return MAP_FAILED;
        }
    } else {
        if (hint >= MMAP_64K_LIMIT)
            hint = 0;
        hint &= ~(uintptr_t)(page_size - 1);
    }

    size_t page_count = mmap_page_count(len, page_size);
    if (page_count > SIZE_MAX / page_size) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    SIZE_T requested_size = page_count * page_size;
    if (hint != 0 && requested_size > range_limit - hint) {
        if (fixed) {
            errno = ENOTSUP;
            return MAP_FAILED;
        }
        hint = 0;
    }
    if (fixed && mmap_check_free(hint, requested_size, page_size) < 0)
        return MAP_FAILED;

    SIZE_T RegionSize = requested_size;
    ULONG AllocationType = MEM_COMMIT | MEM_RESERVE;
    if (prot == PROT_NONE)
        AllocationType = MEM_RESERVE;
    if (page_size == MMAP_LARGE_PAGE_SIZE)
        AllocationType |= MEM_LARGE_PAGES;

    PVOID BaseAddress = NULL;
    NTSTATUS status;
    if (hint != 0) {
        BaseAddress = (PVOID)hint;
        /* Reserve separately so an occupied hint cannot be overwritten. */
        status = NtAllocateVirtualMemory(&BaseAddress, &RegionSize,
                                         MEM_RESERVE, Protect, 0);
        if (NT_SUCCESS(status)) {
            if (BaseAddress == NULL) {
                errno = EIO;
                return MAP_FAILED;
            }

            if ((fixed && (uintptr_t)BaseAddress != hint) ||
                (uintptr_t)BaseAddress % page_size != 0 ||
                (uintptr_t)BaseAddress < range_start ||
                (uintptr_t)BaseAddress >= range_limit ||
                RegionSize > range_limit - (uintptr_t)BaseAddress ||
                RegionSize < requested_size)
            {
                mmap_release_untracked(BaseAddress);
                errno = EIO;
                return MAP_FAILED;
            }

            if (prot != PROT_NONE) {
                PVOID commit_base = BaseAddress;
                SIZE_T commit_size = requested_size;
                status = NtAllocateVirtualMemory(&commit_base, &commit_size,
                                                 MEM_COMMIT, Protect, 0);

                if (!NT_SUCCESS(status)) {
                    mmap_release_untracked(BaseAddress);
                    errno = nt_status_to_errno(status);
                    return MAP_FAILED;
                }

                if (commit_base != BaseAddress || commit_size < requested_size) {
                    mmap_release_untracked(BaseAddress);
                    errno = EIO;
                    return MAP_FAILED;
                }
            }
        } else {
            if (fixed) {
                errno = nt_status_to_errno(status);
                return MAP_FAILED;
            }
            BaseAddress = NULL;
            RegionSize = requested_size;
            status = NtAllocateVirtualMemory(&BaseAddress, &RegionSize,
                                             AllocationType, Protect, 0);
        }
    } else {
        status = NtAllocateVirtualMemory(&BaseAddress, &RegionSize,
                                         AllocationType, Protect, 0);
    }

    if (!NT_SUCCESS(status)) {
        errno = nt_status_to_errno(status);
        return MAP_FAILED;
    }

    if (BaseAddress == NULL) {
        errno = EIO;
        return MAP_FAILED;
    }

    if (RegionSize < requested_size) {
        mmap_release_untracked(BaseAddress);
        errno = ENOMEM;
        return MAP_FAILED;
    }

    if ((uintptr_t)BaseAddress % page_size != 0 ||
        (uintptr_t)BaseAddress < range_start ||
        (uintptr_t)BaseAddress >= range_limit ||
        RegionSize > range_limit - (uintptr_t)BaseAddress) 
    {
        mmap_release_untracked(BaseAddress);
        errno = EIO;
        return MAP_FAILED;
    }

    size_t bitmap_size = (page_count + CHAR_BIT - 1) / CHAR_BIT;
    unsigned char *holes = ExAllocatePoolWithTag(2 * bitmap_size, 0x4d4d6170);
    if (!holes) {
        mmap_release_untracked(BaseAddress);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    unsigned char *committed = holes + bitmap_size;
    memset(holes, 0, bitmap_size);
    memset(committed, prot == PROT_NONE ? 0 : UCHAR_MAX, bitmap_size);

    RtlEnterCriticalSection(&mmap_lock);
    for (int i = 0; i < MMAP_MAX_ALLOC; i++) {
        mmap_info *info = &s_mmap_infos[i];
        if (info->base_address == NULL) {
            info->base_address      = BaseAddress;
            info->page_size         = page_size;
            info->page_count        = page_count;
            info->live_pages        = page_count;
            info->holes             = holes;
            info->committed         = committed;
            RtlLeaveCriticalSection(&mmap_lock);
            return BaseAddress;
        }
    }
    RtlLeaveCriticalSection(&mmap_lock);

    ExFreePool(holes);
    mmap_release_untracked(BaseAddress);
    errno = ENOMEM;
    return MAP_FAILED;
}

int munmap(void *addr, size_t len) {
    uintptr_t start = (uintptr_t)addr;
    size_t page_size = mmap_page_size(start);
    if (addr == NULL || len == 0 || start % page_size != 0) {
        errno = EINVAL;
        return -1;
    }

    size_t count = mmap_page_count(len, page_size);
    size_t first;
    int ret = -1;
    RtlEnterCriticalSection(&mmap_lock);
    mmap_info *info = mmap_find_range(start, count, &first);

    if (info == NULL) {
        errno = EINVAL;
        goto out;
    }

    size_t end = first + count;
    size_t live = 0;
    for (size_t page = first; page < end; page++)
        live += mmap_page_is_live(info, page);

    if (live == info->live_pages) {
        PVOID base = info->base_address;
        SIZE_T size = 0;
        if (!NT_SUCCESS(NtFreeVirtualMemory(&base, &size, MEM_RELEASE, 0))) {
            errno = EIO;
            goto out;
        }
        ExFreePool(info->holes);
        memset(info, 0, sizeof(*info));
        ret = 0;
        goto out;
    } 

    for (size_t page = first; page < end;) {
        if (!mmap_page_is_live(info, page)) {
            page++;
            continue;
        }

        size_t run = page;
        int committed = mmap_bitmap_test(info->committed, page);
        do {
            page++;
        } while (page < end && mmap_page_is_live(info, page) &&
                 mmap_bitmap_test(info->committed, page) == committed);

        if (committed) {
            void *run_base = (char *)info->base_address + run * info->page_size;
            SIZE_T run_size = (page - run) * info->page_size;
            PVOID base = run_base;
            SIZE_T size = run_size;
            /* Xenia's decommit does not revoke host access. */
            if (!NT_SUCCESS(
                NtProtectVirtualMemory(&base, &size, PAGE_NOACCESS, NULL, 0)
            )) {
                errno = EIO;
                goto out;
            }

            base = run_base;
            size = run_size;
            if (!NT_SUCCESS(NtFreeVirtualMemory(&base, &size, MEM_DECOMMIT, 0))) {
                errno = EIO;
                goto out;
            }
        }
        info->live_pages -= page - run;
        for (; run < page; run++)
            mmap_bitmap_set(info->holes, run);
    }
    ret = 0;

out:
    RtlLeaveCriticalSection(&mmap_lock);
    return ret;
}

int mprotect(void *addr, size_t len, int prot) {
    uintptr_t start = (uintptr_t)addr;
    size_t page_size = mmap_page_size(start);
    if (addr == NULL || len == 0 || start % page_size != 0) {
        errno = EINVAL;
        return -1;
    }

    ULONG protect;
    if (posix_prot_to_nt(prot, &protect) < 0)
        return -1;

    size_t count = mmap_page_count(len, page_size);
    size_t first;
    int ret = -1;
    RtlEnterCriticalSection(&mmap_lock);
    mmap_info *info = mmap_find_range(start, count, &first);
    if (info == NULL) {
        errno = ENOMEM;
        goto out;
    }

    size_t end = first + count;
    for (size_t page = first; page < end; page++) {
        if (!mmap_page_is_live(info, page)) {
            errno = ENOMEM;
            goto out;
        }
    }

    for (size_t page = first; page < end;) {
        size_t run = page;
        int committed = mmap_bitmap_test(info->committed, page);
        do {
            page++;
        } while (page < end && mmap_bitmap_test(info->committed, page) == committed);

        if (!committed && prot == PROT_NONE)
            continue;

        PVOID base = (char *)info->base_address + run * info->page_size;
        SIZE_T size = (page - run) * info->page_size;
        NTSTATUS status;
        if (committed)
            status = NtProtectVirtualMemory(&base, &size, protect, NULL, 0);
        else
            status = NtAllocateVirtualMemory(&base, &size, MEM_COMMIT, protect, 0);

        if (!NT_SUCCESS(status)) {
            errno = nt_status_to_errno(status);
            goto out;
        }

        if (!committed)
            for (; run < page; run++)
                mmap_bitmap_set(info->committed, run);
    }
    ret = 0;

out:
    RtlLeaveCriticalSection(&mmap_lock);
    return ret;
}

int madvise(void *addr, size_t len, int advice) {
    if (addr == NULL || len == 0) {
        errno = EINVAL;
        return -1;
    }
    
    switch (advice) {
    default:
        errno = EINVAL;
        return -1;
    case POSIX_MADV_NORMAL:
    case POSIX_MADV_RANDOM:
    case POSIX_MADV_SEQUENTIAL:
    case POSIX_MADV_WILLNEED:
    case POSIX_MADV_DONTNEED: 
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}