#define _GNU_SOURCE
#include "../core/src/host/mg_host.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf target;
static volatile sig_atomic_t reason, code;

static bool MG_HOSTCB record_fault(mg_fault *fault)
{
    reason = fault->diagnostic_segv_reason;
    code = fault->diagnostic_segv_code;
    siglongjmp(target, 1);
    return false;
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    volatile unsigned char *page;
    int failures = 0;
    if (page_size <= 0) return 1;
    page = mmap(NULL, (size_t)page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return 1;
    mg_fault_set_handler(record_fault);
    if (!sigsetjmp(target, 1)) (void)*page;
    if (reason != MG_FAULT_SEGV_ACCERR || code != SEGV_ACCERR) failures++;
    if (munmap((void *)page, (size_t)page_size) != 0) return 1;
    reason = code = 0;
    if (!sigsetjmp(target, 1)) (void)*page;
    if (reason != MG_FAULT_SEGV_MAPERR || code != SEGV_MAPERR) failures++;
    reason = code = 0;
    if (!sigsetjmp(target, 1)) raise(SIGSEGV);
    if (reason != MG_FAULT_SEGV_OTHER || code != SI_TKILL) failures++;
    if (failures) fprintf(stderr, "fault reason checks failed: %d\n", failures);
    return failures ? 1 : 0;
}
