#ifndef MG_FAULT_AARCH64_H
#define MG_FAULT_AARCH64_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { MG_FAULT_DIAG_NONE, MG_FAULT_DIAG_UNKNOWN, MG_FAULT_DIAG_READ, MG_FAULT_DIAG_WRITE, MG_FAULT_DIAG_EXEC };

/* Decode only records in sigcontext.__reserved; extra context is not followed. */
static inline int mg_aarch64_fault_access(const void *records, size_t length)
{
    const unsigned char *p = (const unsigned char *)records;
    while (length >= 8) {
        uint32_t magic, size;
        uint64_t esr;
        unsigned ec;
        memcpy(&magic, p, sizeof(magic));
        memcpy(&size, p + 4, sizeof(size));
        if (!magic && !size) break;
        if (size < 16 || (size & 15) || size > length) break;
        if (magic == 0x45535201u) {
            memcpy(&esr, p + 8, sizeof(esr));
            ec = (unsigned)((esr >> 26) & 0x3f);
            if (ec == 0x20 || ec == 0x21) return MG_FAULT_DIAG_EXEC;
            if ((ec == 0x24 || ec == 0x25) && !(esr & (1u << 8)))
                return (esr & (1u << 6)) ? MG_FAULT_DIAG_WRITE : MG_FAULT_DIAG_READ;
            break;
        }
        p += size;
        length -= size;
    }
    return MG_FAULT_DIAG_UNKNOWN;
}

#endif
