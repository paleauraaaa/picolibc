/* Standalone Xbox guest test; link with the Xbox CRT and kernel/XAM imports. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <xboxkrnl.h>

#define PAGE_SIZE 4096
#define LARGE_PAGE_SIZE 65536
#define LARGE_PAGE_FLAG 0x20000000u
#define FLAGS (MAP_PRIVATE | MAP_ANONYMOUS)

unsigned int _tls_index;

static int
check(int condition, const char *message)
{
    if (!condition)
        printf("mmap hint test failed: %s (errno %d)\n", message, errno);
    return condition;
}

#ifdef TEST_MMAP_FIXED_FAULTS
static int fault_mode;
static int query_count;
static int injection_failed;
static void *race_base;
static void *wrong_fallback;
static size_t fault_page_size;

ULONG NTAPI
mmap_test_query(PVOID address, X_MEMORY_BASIC_INFORMATION *info, ULONG type)
{
    ++query_count;
    if (fault_mode == 1)
        return STATUS_ACCESS_DENIED;
    ULONG status = NtQueryVirtualMemory(address, info, type);
    if (!NT_SUCCESS(status) || info->State != MEM_FREE)
        return status;
    if (fault_mode == 2) {
        SIZE_T size = fault_page_size;
        race_base = address;
        status = NtAllocateVirtualMemory(&race_base, &size, MEM_RESERVE, PAGE_READWRITE, 0);
        if (NT_SUCCESS(status)) {
            PVOID commit_base = race_base;
            size = fault_page_size;
            status = NtAllocateVirtualMemory(&commit_base, &size, MEM_COMMIT, PAGE_READWRITE, 0);
        }
        if (!NT_SUCCESS(status) || race_base != address)
            injection_failed = 1;
        else
            *(unsigned char *)race_base = 0x36;
    } else if (fault_mode == 3) {
        info->BaseAddress = address;
        info->RegionSize = fault_page_size;
    } else if (fault_mode == 4) {
        info->RegionSize = 0;
    }
    return STATUS_SUCCESS;
}

ULONG NTAPI
mmap_test_allocate(PVOID base, PSIZE_T size, ULONG type, ULONG protect, ULONG debug)
{
    if (fault_mode == 5 && type == MEM_COMMIT)
        return STATUS_NO_MEMORY;
    if (fault_mode == 6 && *(PVOID *)base == NULL)
        return STATUS_NO_MEMORY;
    if (fault_mode == 7 && *(PVOID *)base == NULL) {
        ULONG status = NtAllocateVirtualMemory(base, size, type & ~LARGE_PAGE_FLAG, protect, debug);
        if (NT_SUCCESS(status))
            wrong_fallback = *(PVOID *)base;
        return status;
    }
    return NtAllocateVirtualMemory(base, size, type, protect, debug);
}

static int
kernel_range_is_free(void *address)
{
    X_MEMORY_BASIC_INFORMATION info;
    return NT_SUCCESS(NtQueryVirtualMemory(address, &info, 0)) && info.State == MEM_FREE;
}

static int
check_fixed_faults(void *address, size_t page_size)
{
    fault_page_size = page_size;
    injection_failed = 0;
    fault_mode = 1;
    errno = 0;
    if (!check(mmap(address, page_size, PROT_READ, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EACCES && kernel_range_is_free(address), "fixed query failure"))
        return 0;
    fault_mode = 4;
    errno = 0;
    if (!check(mmap(address, page_size, PROT_READ, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EIO && kernel_range_is_free(address), "invalid query extent"))
        return 0;
    fault_mode = 3;
    query_count = 0;
    void *p = mmap(address, 2 * page_size, PROT_READ | PROT_WRITE, FLAGS | MAP_FIXED, -1, 0);
    if (!check(p == address && query_count == 2, "multiple free query regions") ||
        !check(munmap(p, 2 * page_size) == 0, "release split-query mapping"))
        return 0;
    fault_mode = 5;
    errno = 0;
    if (!check(mmap(address, page_size, PROT_READ | PROT_WRITE, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == ENOMEM && kernel_range_is_free(address), "fixed commit failure cleanup"))
        return 0;
    fault_mode = 2;
    if (!check(mmap(address, page_size, PROT_READ | PROT_WRITE, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               !injection_failed && race_base == address && *(unsigned char *)race_base == 0x36,
               "racing reservation preserved"))
        return 0;
    SIZE_T size = 0;
    if (!check(NT_SUCCESS(NtFreeVirtualMemory(&race_base, &size, MEM_RELEASE, 0)),
               "release racing reservation"))
        return 0;
    fault_mode = 0;
    return 1;
}
#endif

static int
check_explicit_large_pages(void)
{
    const uintptr_t invalid[] = { 1, 0x10000, 0x3fffffff, 0x80000000, 0x90000000 };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        errno = 0;
        if (!check(mmap((void *)invalid[i], 1, PROT_READ, FLAGS | MAP_LARGE_PAGES, -1, 0) == MAP_FAILED &&
                   errno == EINVAL, "large page flag rejects out-of-range hint"))
            return 0;
    }
    errno = 0;
    if (!check(mmap((void *)0x10000, 1, PROT_READ, FLAGS | MAP_LARGE_PAGES | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EINVAL, "large page flag rejects out-of-range fixed address"))
        return 0;
    errno = 0;
    if (!check(mmap((void *)0x80000000, 1, PROT_READ, FLAGS | MAP_LARGE_PAGES | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EINVAL, "large page flag rejects high fixed address"))
        return 0;
    errno = 0;
    if (!check(mmap(NULL, 1, PROT_READ, FLAGS | MAP_LARGE_PAGES | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EINVAL, "large page flag rejects null fixed address"))
        return 0;

#ifdef TEST_MMAP_FIXED_FAULTS
    fault_mode = 6;
    errno = 0;
    if (!check(mmap(NULL, 1, PROT_READ, FLAGS | MAP_LARGE_PAGES, -1, 0) == MAP_FAILED &&
               errno == ENOMEM, "unhinted large page allocation failure"))
        return 0;
    fault_mode = 7;
    wrong_fallback = NULL;
    errno = 0;
    if (!check(mmap(NULL, 1, PROT_READ | PROT_WRITE, FLAGS | MAP_LARGE_PAGES, -1, 0) == MAP_FAILED &&
               errno == EIO && wrong_fallback != NULL && kernel_range_is_free(wrong_fallback),
               "unhinted large page allocation rejects wrong range"))
        return 0;
    fault_mode = 0;
#endif

    unsigned char *p = mmap(NULL, LARGE_PAGE_SIZE + 1, PROT_READ | PROT_WRITE,
                            FLAGS | MAP_LARGE_PAGES, -1, 0);
    if (!check(p != MAP_FAILED && (uintptr_t)p >= 0x40000000 &&
               (uintptr_t)p < 0x80000000 && (uintptr_t)p % LARGE_PAGE_SIZE == 0,
               "unhinted large page allocation") ||
        !check(p[0] == 0 && p[2 * LARGE_PAGE_SIZE - 1] == 0, "unhinted large pages zero-filled"))
        return 0;
    p[0] = 0x6a;
    p[LARGE_PAGE_SIZE] = 0x7b;
    errno = 0;
    if (!check(munmap(p + PAGE_SIZE, 1) == -1 && errno == EINVAL,
               "unhinted large page alignment") ||
        !check(munmap(p + LARGE_PAGE_SIZE, 1) == 0, "unmap second unhinted large page") ||
        !check(p[0] == 0x6a, "unhinted large page neighbor preserved") ||
        !check(munmap(p, 1) == 0, "unmap first unhinted large page"))
        return 0;

    void *reserved = mmap(NULL, 1, PROT_NONE, FLAGS | MAP_LARGE_PAGES, -1, 0);
    if (!check(reserved != MAP_FAILED && (uintptr_t)reserved >= 0x40000000 &&
               (uintptr_t)reserved < 0x80000000, "unhinted large page reservation") ||
        !check(mprotect(reserved, 1, PROT_READ | PROT_WRITE) == 0, "commit unhinted large page") ||
        !check(((unsigned char *)reserved)[LARGE_PAGE_SIZE - 1] == 0, "unhinted reservation zero-filled") ||
        !check(munmap(reserved, 1) == 0, "release unhinted large page"))
        return 0;
    return 1;
}

static int
check_large_pages(void)
{
    uintptr_t hint;
    for (hint = 0x50000000; hint < 0x7e000000; hint += LARGE_PAGE_SIZE) {
        X_MEMORY_BASIC_INFORMATION info;
        if (NT_SUCCESS(NtQueryVirtualMemory((void *)hint, &info, 0)) &&
            info.State == MEM_FREE && (uintptr_t)info.BaseAddress <= hint &&
            info.RegionSize >= hint - (uintptr_t)info.BaseAddress &&
            info.RegionSize - (hint - (uintptr_t)info.BaseAddress) >= 3 * LARGE_PAGE_SIZE)
            break;
    }
    if (!check(hint < 0x7e000000, "find free 64 KiB range"))
        return 0;

#ifdef TEST_MMAP_FIXED_FAULTS
    if (!check_fixed_faults((void *)hint, LARGE_PAGE_SIZE))
        return 0;
#endif

    unsigned char *first = mmap((void *)(hint + 17), 3 * LARGE_PAGE_SIZE - 1,
                                PROT_READ | PROT_WRITE, FLAGS, -1, 0);
    if (!check(first == (void *)hint, "64 KiB hint") ||
        !check(first[0] == 0 && first[3 * LARGE_PAGE_SIZE - 1] == 0,
               "64 KiB mapping zero-filled"))
        return 0;
    first[0] = 0x5a;
    first[2 * LARGE_PAGE_SIZE] = 0xa5;
#ifdef TEST_MMAP_FIXED_FAULTS
    fault_mode = 6;
    errno = 0;
    if (!check(mmap((void *)hint, 1, PROT_READ | PROT_WRITE, FLAGS, -1, 0) == MAP_FAILED &&
               errno == ENOMEM && first[0] == 0x5a, "64 KiB fallback failure"))
        return 0;
    fault_mode = 7;
    errno = 0;
    if (!check(mmap((void *)hint, 1, PROT_READ | PROT_WRITE, FLAGS, -1, 0) == MAP_FAILED &&
               errno == EIO && wrong_fallback != NULL && kernel_range_is_free(wrong_fallback),
               "wrong-page-size fallback rejected and released"))
        return 0;
    fault_mode = 0;
#endif
    errno = 0;
    if (!check(mmap((void *)hint, 1, PROT_READ, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EEXIST, "occupied 64 KiB fixed mapping"))
        return 0;
    errno = 0;
    if (!check(munmap(first + PAGE_SIZE, PAGE_SIZE) == -1 && errno == EINVAL,
               "64 KiB unmap alignment"))
        return 0;
    errno = 0;
    if (!check(mprotect(first + PAGE_SIZE, PAGE_SIZE, PROT_NONE) == -1 && errno == EINVAL,
               "64 KiB protection alignment"))
        return 0;
    if (!check(mprotect(first + 2 * LARGE_PAGE_SIZE, 1, PROT_NONE) == 0,
               "64 KiB revoke protection") ||
        !check(mprotect(first + 2 * LARGE_PAGE_SIZE, 1, PROT_READ | PROT_WRITE) == 0,
               "64 KiB restore protection") ||
        !check(first[2 * LARGE_PAGE_SIZE] == 0xa5, "64 KiB protection preserves contents"))
        return 0;

    unsigned char *fallback = mmap((void *)hint, LARGE_PAGE_SIZE + 1,
                                   PROT_READ | PROT_WRITE, FLAGS, -1, 0);
    if (!check(fallback != MAP_FAILED && (uintptr_t)fallback >= 0x40000000 &&
               (uintptr_t)fallback < 0x80000000 &&
               (uintptr_t)fallback % LARGE_PAGE_SIZE == 0 && fallback != first,
               "occupied 64 KiB hint keeps 64 KiB pages"))
        return 0;
    errno = 0;
    if (!check(munmap(fallback + PAGE_SIZE, PAGE_SIZE) == -1 && errno == EINVAL,
               "fallback rejects 4 KiB-aligned unmap") ||
        !check(munmap(fallback + LARGE_PAGE_SIZE, 1) == 0, "release fallback second page") ||
        !check(munmap(fallback, 1) == 0, "release fallback first page") ||
        !check(first[0] == 0x5a, "occupied 64 KiB mapping preserved"))
        return 0;

    void *fallback_reserved = mmap((void *)hint, 1, PROT_NONE, FLAGS | MAP_LARGE_PAGES, -1, 0);
    if (!check(fallback_reserved != MAP_FAILED && (uintptr_t)fallback_reserved >= 0x40000000 &&
               (uintptr_t)fallback_reserved < 0x80000000 &&
               (uintptr_t)fallback_reserved % LARGE_PAGE_SIZE == 0,
               "64 KiB PROT_NONE fallback") ||
        !check(mprotect(fallback_reserved, 1, PROT_READ | PROT_WRITE) == 0,
               "commit 64 KiB fallback") ||
        !check(((unsigned char *)fallback_reserved)[LARGE_PAGE_SIZE - 1] == 0,
               "64 KiB fallback zero-filled") ||
        !check(munmap(fallback_reserved, 1) == 0, "release 64 KiB PROT_NONE fallback"))
        return 0;

    if (!check(munmap(first + LARGE_PAGE_SIZE, 1) == 0, "partial 64 KiB unmap") ||
        !check(first[0] == 0x5a && first[2 * LARGE_PAGE_SIZE] == 0xa5,
               "64 KiB neighbors preserved"))
        return 0;
    errno = 0;
    if (!check(mprotect(first + LARGE_PAGE_SIZE, 1, PROT_READ) == -1 && errno == ENOMEM,
               "64 KiB hole rejects protection"))
        return 0;
    errno = 0;
    if (!check(mmap(first + LARGE_PAGE_SIZE, 1, PROT_READ, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EEXIST, "64 KiB reserved hole rejects fixed mapping") ||
        !check(munmap(first, 1) == 0, "release first 64 KiB page") ||
        !check(munmap(first + 2 * LARGE_PAGE_SIZE, 1) == 0, "release last 64 KiB page"))
        return 0;

    void *reserved = mmap((void *)hint, LARGE_PAGE_SIZE + 1, PROT_NONE,
                          FLAGS | MAP_FIXED | MAP_LARGE_PAGES, -1, 0);
    if (!check(reserved == (void *)hint, "64 KiB fixed reservation") ||
        !check(mprotect((char *)reserved + LARGE_PAGE_SIZE, 1, PROT_READ | PROT_WRITE) == 0,
               "commit second 64 KiB page") ||
        !check(((unsigned char *)reserved)[2 * LARGE_PAGE_SIZE - 1] == 0,
               "64 KiB commit zero-filled"))
        return 0;
    ((unsigned char *)reserved)[2 * LARGE_PAGE_SIZE - 1] = 0x7c;
    if (!check(mprotect(reserved, 2 * LARGE_PAGE_SIZE, PROT_READ) == 0,
               "protect mixed 64 KiB pages") ||
        !check(((unsigned char *)reserved)[0] == 0 &&
               ((unsigned char *)reserved)[2 * LARGE_PAGE_SIZE - 1] == 0x7c,
               "mixed 64 KiB pages preserve contents") ||
        !check(munmap(reserved, LARGE_PAGE_SIZE + 1) == 0, "release 64 KiB fixed mapping"))
        return 0;
    void *flagged_hint = mmap((void *)(hint + 17), 1, PROT_READ | PROT_WRITE,
                              FLAGS | MAP_LARGE_PAGES, -1, 0);
    if (!check(flagged_hint == (void *)hint, "explicit large page hint") ||
        !check(munmap(flagged_hint, 1) == 0, "release explicit large page hint"))
        return 0;
    errno = 0;
    if (!check(mmap((void *)(hint + PAGE_SIZE), 1, PROT_READ, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EINVAL, "64 KiB fixed alignment"))
        return 0;
    return 1;
}

int
main(void)
{
    void *base = mmap(NULL, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE, FLAGS, -1, 0);
    if (!check(base != MAP_FAILED, "initial mapping"))
        return 1;
    uintptr_t hint = (uintptr_t)base + PAGE_SIZE;
    if (!check(munmap(base, 3 * PAGE_SIZE) == 0, "release initial mapping"))
        return 1;

#ifdef TEST_MMAP_FIXED_FAULTS
    if (!check_fixed_faults((void *)hint, PAGE_SIZE))
        return 1;
#endif

    unsigned char *first = mmap((void *)(hint + 17), PAGE_SIZE,
                                PROT_READ | PROT_WRITE, FLAGS, -1, 0);
    if (!check(first == (void *)hint, "available unaligned hint") ||
        !check(first[0] == 0, "zero-filled hinted mapping"))
        return 1;
    first[0] = 0x5a;

    void *second = mmap((void *)hint, PAGE_SIZE, PROT_READ | PROT_WRITE, FLAGS, -1, 0);
    if (!check(second != MAP_FAILED && second != first, "occupied hint fallback") ||
        !check(first[0] == 0x5a, "occupied mapping preserved") ||
        !check(munmap(second, PAGE_SIZE) == 0, "release fallback") ||
        !check(munmap(first, PAGE_SIZE) == 0, "release hinted mapping"))
        return 1;

    void *reserved = mmap((void *)hint, PAGE_SIZE, PROT_NONE, FLAGS, -1, 0);
    if (!check(reserved == (void *)hint, "hinted reservation") ||
        !check(mprotect(reserved, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
               "commit hinted reservation") ||
        !check(((unsigned char *)reserved)[0] == 0, "zero-filled hinted reservation") ||
        !check(munmap(reserved, PAGE_SIZE) == 0, "release hinted reservation"))
        return 1;

    first = mmap((void *)hint, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE, FLAGS, -1, 0);
    if (!check(first == (void *)hint, "multi-page hinted mapping"))
        return 1;
    first[0] = 0xa5;
    if (!check(munmap(first + PAGE_SIZE, PAGE_SIZE) == 0, "partial hinted unmap"))
        return 1;
    errno = 0;
    if (!check(mmap(first + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE,
                    FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED && errno == EEXIST,
               "fixed mapping rejects reserved hole"))
        return 1;
    second = mmap(first + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE, FLAGS, -1, 0);
    if (!check(second != MAP_FAILED && second != first + PAGE_SIZE, "reserved hole fallback") ||
        !check(first[0] == 0xa5, "partial unmap preserves live page") ||
        !check(munmap(second, PAGE_SIZE) == 0, "release hole fallback") ||
        !check(munmap(first, PAGE_SIZE) == 0, "release last hinted page"))
        return 1;

    const uintptr_t ignored[] = { 1, 0x3ffff000, 0x90000000 };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        void *mapping = mmap((void *)ignored[i], 2 * PAGE_SIZE,
                             PROT_READ | PROT_WRITE, FLAGS, -1, 0);
        if (!check(mapping != MAP_FAILED && (uintptr_t)mapping < 0x40000000,
                   "unavailable or ignored hint fallback") ||
            !check((uintptr_t)mapping != ignored[i], "ignored hint not used") ||
            !check(munmap(mapping, 2 * PAGE_SIZE) == 0, "release ignored-hint mapping"))
            return 1;
    }
    void *crossing = mmap((void *)0x7fff0000, 2 * LARGE_PAGE_SIZE,
                          PROT_READ | PROT_WRITE, FLAGS, -1, 0);
    if (!check(crossing != MAP_FAILED && (uintptr_t)crossing >= 0x40000000 &&
               (uintptr_t)crossing < 0x80000000 &&
               (uintptr_t)crossing % LARGE_PAGE_SIZE == 0,
               "crossing 64 KiB hint keeps 64 KiB pages") ||
        !check(munmap(crossing, 2 * LARGE_PAGE_SIZE) == 0, "release crossing fallback"))
        return 1;
    crossing = mmap((void *)0x7fff0000, 2 * LARGE_PAGE_SIZE,
                    PROT_READ | PROT_WRITE, FLAGS | MAP_LARGE_PAGES, -1, 0);
    if (!check(crossing != MAP_FAILED && (uintptr_t)crossing >= 0x40000000 &&
               (uintptr_t)crossing < 0x80000000,
               "explicit crossing hint keeps 64 KiB pages") ||
        !check(munmap(crossing, 2 * LARGE_PAGE_SIZE) == 0, "release explicit crossing fallback"))
        return 1;

    first = mmap((void *)hint, PAGE_SIZE - 1, PROT_READ | PROT_WRITE,
                 FLAGS | MAP_FIXED, -1, 0);
    if (!check(first == (void *)hint, "fixed mapping at free address") ||
        !check(first[0] == 0, "fixed mapping is zero-filled"))
        return 1;
    first[0] = 0x6b;
    errno = 0;
    if (!check(mmap((void *)hint, PAGE_SIZE, PROT_READ | PROT_WRITE,
                    FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED && errno == EEXIST,
               "fixed mapping rejects occupied address") ||
        !check(first[0] == 0x6b, "fixed collision preserves mapping") ||
        !check(munmap(first, PAGE_SIZE) == 0, "release fixed mapping"))
        return 1;

    first = mmap((void *)(hint + PAGE_SIZE), PAGE_SIZE, PROT_READ | PROT_WRITE,
                 FLAGS | MAP_FIXED, -1, 0);
    if (!check(first == (void *)(hint + PAGE_SIZE), "fixed second page"))
        return 1;
    first[0] = 0x7c;
    errno = 0;
    if (!check(mmap((void *)hint, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE,
                    FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED && errno == EEXIST,
               "fixed range rejects occupied later page") ||
        !check(first[0] == 0x7c, "fixed overlapping range preserves mapping"))
        return 1;
    second = mmap((void *)hint, PAGE_SIZE, PROT_NONE, FLAGS | MAP_FIXED, -1, 0);
    if (!check(second == (void *)hint, "fixed PROT_NONE reservation") ||
        !check(mprotect(second, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
               "commit fixed reservation") ||
        !check(((unsigned char *)second)[0] == 0, "fixed reservation is zero-filled") ||
        !check(munmap(second, PAGE_SIZE) == 0, "release fixed reservation") ||
        !check(munmap(first, PAGE_SIZE) == 0, "release fixed second page"))
        return 1;

    PVOID kernel_base = (void *)hint;
    SIZE_T kernel_size = PAGE_SIZE;
    if (!check(NT_SUCCESS(NtAllocateVirtualMemory(&kernel_base, &kernel_size,
                                                  MEM_RESERVE, PAGE_NOACCESS, 0)) &&
               kernel_base == (void *)hint, "reserve through kernel"))
        return 1;
    errno = 0;
    if (!check(mmap((void *)hint, PAGE_SIZE, PROT_READ, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EEXIST, "fixed mapping rejects external reservation"))
        return 1;
    kernel_size = 0;
    if (!check(NT_SUCCESS(NtFreeVirtualMemory(&kernel_base, &kernel_size, MEM_RELEASE, 0)),
               "release external reservation"))
        return 1;

    errno = 0;
    if (!check(mmap((void *)(hint + 1), PAGE_SIZE, PROT_READ, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EINVAL, "fixed mapping rejects unaligned address"))
        return 1;
    errno = 0;
    if (!check(mmap(NULL, PAGE_SIZE, PROT_READ, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == EINVAL, "fixed mapping rejects null address"))
        return 1;
    errno = 0;
    if (!check(mmap((void *)0x3ffff000, 2 * PAGE_SIZE, PROT_READ, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == ENOTSUP, "fixed mapping rejects crossing range"))
        return 1;
    errno = 0;
    if (!check(mmap((void *)0x7fff0000, 2 * LARGE_PAGE_SIZE, PROT_READ, FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED &&
               errno == ENOTSUP, "fixed mapping rejects end of 64 KiB range") ||
        !check(check_explicit_large_pages(), "explicit 64 KiB page tests") ||
        !check(check_large_pages(), "64 KiB page tests"))
        return 1;
    puts("xbox mmap hint test passed");
    return 0;
}
