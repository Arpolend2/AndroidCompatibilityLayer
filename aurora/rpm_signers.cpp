#include "rpm_signers.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstring>

namespace RpmSigners {
namespace {

typedef void *(*BioMemBufFn)(const void *, int);
typedef void *(*PemReadFn)(void *, void **, void *, void *);
typedef int (*BioFreeFn)(void *);
typedef int (*InitFn)(unsigned long, const void *);
typedef void (*VoidFn)();
typedef void *(*ByIdFn)(const char *);
typedef int (*EngIntFn)(void *);
typedef int (*EngDefaultFn)(void *, unsigned int);
typedef unsigned long (*ErrFn)();
typedef void (*ErrStrFn)(unsigned long, char *, size_t);
typedef bool (*SignFn)(void *, void *, void *, void *, unsigned int, void *, bool, bool, bool);

struct Crypto {
    void *h = nullptr;
    BioMemBufFn memBuf = nullptr;
    PemReadFn readX509 = nullptr, readKey = nullptr;
    BioFreeFn bioFree = nullptr;
    ErrFn err = nullptr;
    ErrStrFn errStr = nullptr;
    void *cert = nullptr, *key = nullptr;
};

template <typename T> T sym(void *h, const char *name)
{
    return reinterpret_cast<T>(dlsym(h, name));
}

std::string lastError(const Crypto &c)
{
    std::string out;
    if (!c.err || !c.errStr) return out;
    while (unsigned long e = c.err()) {
        char buf[256] = {0};
        c.errStr(e, buf, sizeof buf);
        out += std::string(" [") + buf + "]";
    }
    return out;
}

bool loadCrypto(Crypto &c, const char *keyPem, const char *certPem, std::string &log)
{
    c.h = dlopen("libcrypto.so.1.1", RTLD_NOW | RTLD_GLOBAL);
    if (!c.h) {
        log += std::string("dlopen libcrypto failed: ") + dlerror() + "\n";
        return false;
    }
    c.memBuf = sym<BioMemBufFn>(c.h, "BIO_new_mem_buf");
    c.readX509 = sym<PemReadFn>(c.h, "PEM_read_bio_X509");
    c.readKey = sym<PemReadFn>(c.h, "PEM_read_bio_PrivateKey");
    c.bioFree = sym<BioFreeFn>(c.h, "BIO_free");
    c.err = sym<ErrFn>(c.h, "ERR_get_error");
    c.errStr = sym<ErrStrFn>(c.h, "ERR_error_string_n");
    if (!c.memBuf || !c.readX509 || !c.readKey || !c.bioFree) {
        log += "libcrypto symbols missing\n";
        return false;
    }
    if (InitFn init = sym<InitFn>(c.h, "OPENSSL_init_crypto")) init(0x4 | 0x8 | 0x40, nullptr);
    VoidFn loadEngines = sym<VoidFn>(c.h, "ENGINE_load_builtin_engines");
    ByIdFn byId = sym<ByIdFn>(c.h, "ENGINE_by_id");
    EngIntFn engInit = sym<EngIntFn>(c.h, "ENGINE_init");
    EngDefaultFn setDefault = sym<EngDefaultFn>(c.h, "ENGINE_set_default");
    if (loadEngines && byId && engInit && setDefault) {
        loadEngines();
        if (void *e = byId("gost")) {
            const int i = engInit(e);
            const int d = setDefault(e, 0xFFFF);
            log += "gost engine init=" + std::to_string(i) + " default=" + std::to_string(d) + "\n";
        } else {
            log += "gost engine not found\n";
        }
    }
    void *cb = c.memBuf(certPem, int(std::strlen(certPem)));
    c.cert = c.readX509(cb, nullptr, nullptr, nullptr);
    c.bioFree(cb);
    void *kb = c.memBuf(keyPem, int(std::strlen(keyPem)));
    c.key = c.readKey(kb, nullptr, nullptr, nullptr);
    c.bioFree(kb);
    if (!c.cert || !c.key) {
        log += std::string("cannot read cert/key: cert=") + (c.cert ? "ok" : "null")
            + " key=" + (c.key ? "ok" : "null") + lastError(c) + "\n";
        return false;
    }
    return true;
}

}

bool signWithLibrary(const std::string &path, const char *keyPem, const char *certPem, std::string &log)
{
    Crypto c;
    if (!loadCrypto(c, keyPem, certPem, log)) return false;
    void *lib = dlopen("librpmsign-external.so.3", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        log += std::string("dlopen librpmsign-external failed: ") + dlerror() + "\n";
        return false;
    }
    SignFn sign = sym<SignFn>(lib,
        "_ZN17PackageSignatures13FileWithCmses4signEP7x509_stP11evp_pkey_stS4_jP24stack_st_ProofOfRotationbbb");
    if (!sign) {
        log += "FileWithCmses::sign symbol not found\n";
        return false;
    }
    // FileWithCmses is a single FILE* member (see its constructor and destructor).
    const bool combos[3][3] = {{false, false, true}, {true, false, true}, {false, true, true}};
    for (int i = 0; i < 3; ++i) {
        FILE *f = std::fopen(path.c_str(), "r+b");
        if (!f) {
            log += "cannot open " + path + "\n";
            return false;
        }
        struct { FILE *file; } object = {f};
        const bool ok = sign(&object, c.cert, c.key, nullptr, 0, nullptr, combos[i][0], combos[i][1], combos[i][2]);
        std::fclose(f);
        log += "FileWithCmses::sign flags " + std::to_string(combos[i][0]) + std::to_string(combos[i][1])
            + std::to_string(combos[i][2]) + " -> " + (ok ? "true" : "false") + lastError(c) + "\n";
        if (ok) return true;
    }
    return false;
}

}
