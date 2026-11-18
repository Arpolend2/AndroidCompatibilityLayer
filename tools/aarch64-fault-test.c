#include "../core/src/host/mg_fault_aarch64.h"

#include <stdio.h>

static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
static void put64(unsigned char *p, uint64_t v) { memcpy(p, &v, sizeof(v)); }

int main(void)
{
    unsigned char records[64] = {0};
    int failures = 0;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "failed: %s\n", #expr); failures++; } } while (0)
    CHECK(mg_aarch64_fault_access(records, 0) == MG_FAULT_DIAG_UNKNOWN);
    CHECK(mg_aarch64_fault_access(records, sizeof(records)) == MG_FAULT_DIAG_UNKNOWN);
    put32(records, 0x46508001u);
    put32(records + 4, 16);
    put32(records + 16, 0x45535201u);
    put32(records + 20, 16);
    put64(records + 24, (uint64_t)0x24 << 26);
    CHECK(mg_aarch64_fault_access(records, sizeof(records)) == MG_FAULT_DIAG_READ);
    put64(records + 24, ((uint64_t)0x25 << 26) | (1u << 6));
    CHECK(mg_aarch64_fault_access(records, sizeof(records)) == MG_FAULT_DIAG_WRITE);
    put64(records + 24, (uint64_t)0x20 << 26);
    CHECK(mg_aarch64_fault_access(records, sizeof(records)) == MG_FAULT_DIAG_EXEC);
    put64(records + 24, (uint64_t)0x21 << 26);
    CHECK(mg_aarch64_fault_access(records, sizeof(records)) == MG_FAULT_DIAG_EXEC);
    put64(records + 24, ((uint64_t)0x24 << 26) | (1u << 8) | (1u << 6));
    CHECK(mg_aarch64_fault_access(records, sizeof(records)) == MG_FAULT_DIAG_UNKNOWN);
    put64(records + 24, (uint64_t)0x2f << 26);
    CHECK(mg_aarch64_fault_access(records, sizeof(records)) == MG_FAULT_DIAG_UNKNOWN);
    put32(records + 20, 32);
    CHECK(mg_aarch64_fault_access(records, 32) == MG_FAULT_DIAG_UNKNOWN);
    put32(records + 20, 0);
    CHECK(mg_aarch64_fault_access(records, sizeof(records)) == MG_FAULT_DIAG_UNKNOWN);
    put32(records + 20, 15);
    CHECK(mg_aarch64_fault_access(records, sizeof(records)) == MG_FAULT_DIAG_UNKNOWN);
    put32(records + 20, 16);
    CHECK(mg_aarch64_fault_access(records, 24) == MG_FAULT_DIAG_UNKNOWN);
    return failures ? 1 : 0;
}
