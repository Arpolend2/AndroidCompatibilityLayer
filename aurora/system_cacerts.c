#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "system_cacerts.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef struct {
    unsigned char *values;
    size_t count;
} fingerprints;

typedef struct {
    const char *directory;
    fingerprints denied;
    fingerprints restricted;
    fingerprints seen;
    SHA256_CTX snapshot;
    size_t written;
    size_t excluded;
    char *error;
    size_t error_size;
} store;

static void fail(store *s, const char *format, ...)
{
    va_list ap;
    if (!s->error_size || s->error[0]) return;
    va_start(ap, format);
    vsnprintf(s->error, s->error_size, format, ap);
    va_end(ap);
}

static char *path_join(const char *a, const char *b)
{
    char *out = NULL;
    if (asprintf(&out, "%s/%s", a, b) < 0) return NULL;
    return out;
}

static int has_fingerprint(const fingerprints *set, const unsigned char *digest)
{
    size_t i;
    for (i = 0; i < set->count; ++i)
        if (!memcmp(set->values + i * SHA256_DIGEST_LENGTH, digest, SHA256_DIGEST_LENGTH)) return 1;
    return 0;
}

static int add_fingerprint(fingerprints *set, const unsigned char *digest)
{
    unsigned char *p;
    if (has_fingerprint(set, digest)) return 1;
    p = realloc(set->values, (set->count + 1) * SHA256_DIGEST_LENGTH);
    if (!p) return 0;
    set->values = p;
    memcpy(p + set->count * SHA256_DIGEST_LENGTH, digest, SHA256_DIGEST_LENGTH);
    ++set->count;
    return 1;
}

static int handle_certificate(store *s, X509 *cert, int kind)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int length = 0;
    unsigned long hash;
    unsigned int index;
    char name[64], *path;
    FILE *file;
    int fd, ok;
    if (!X509_digest(cert, EVP_sha256(), digest, &length) || length != sizeof(digest)) {
        fail(s, "cannot fingerprint certificate");
        return 0;
    }
    if (kind == 1) {
        if (!add_fingerprint(&s->denied, digest)) {
            fail(s, "out of memory reading certificate blacklist");
            return 0;
        }
        return 1;
    }
    if (has_fingerprint(&s->denied, digest)) {
        ++s->excluded;
        return 1;
    }
    if (has_fingerprint(&s->seen, digest)) return 1;
    if (kind == 2 && has_fingerprint(&s->restricted, digest)) {
        ++s->excluded;
        return 1;
    }
    if (!add_fingerprint(&s->seen, digest)) {
        fail(s, "out of memory merging certificates");
        return 0;
    }
    hash = X509_NAME_hash_old(X509_get_subject_name(cert));
    for (index = 0; ; ++index) {
        snprintf(name, sizeof(name), "%08lx.%u", hash & 0xffffffffUL, index);
        path = path_join(s->directory, name);
        if (!path) {
            fail(s, "out of memory creating certificate path");
            return 0;
        }
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) break;
        free(path);
        if (errno != EEXIST) {
            fail(s, "cannot create certificate %s: %s", name, strerror(errno));
            return 0;
        }
    }
    file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        fail(s, "cannot open certificate %s: %s", name, strerror(errno));
        free(path);
        return 0;
    }
    ok = PEM_write_X509(file, cert) == 1;
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        fail(s, "cannot write certificate %s", name);
        free(path);
        return 0;
    }
    SHA256_Update(&s->snapshot, name, strlen(name) + 1);
    SHA256_Update(&s->snapshot, digest, sizeof(digest));
    ++s->written;
    free(path);
    return 1;
}

static int read_certificates(store *s, const char *path, int kind, int allow_empty, size_t *parsed)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----";
    static const char end[] = "-----END CERTIFICATE-----";
    FILE *file = fopen(path, "rb");
    char *data = NULL, *cursor;
    long length;
    size_t count = 0;
    int ok = 0;
    if (!file) {
        fail(s, "cannot read %s: %s", path, strerror(errno));
        return 0;
    }
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || length > 16 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET) || !(data = malloc((size_t)length + 1)) ||
        fread(data, 1, (size_t)length, file) != (size_t)length) {
        fail(s, "cannot read certificate data from %s", path);
        goto done;
    }
    data[length] = 0;
    if (kind == 1 && strstr(data, "-----BEGIN TRUSTED CERTIFICATE-----")) {
        fail(s, "unsupported trusted certificate in blacklist %s", path);
        goto done;
    }
    cursor = data;
    for (;;) {
        char *start = strstr(cursor, begin), *finish;
        BIO *bio;
        X509 *cert;
        if (!start) break;
        finish = strstr(start + sizeof(begin) - 1, end);
        if (!finish) {
            fail(s, "incomplete certificate in %s", path);
            goto done;
        }
        finish += sizeof(end) - 1;
        bio = BIO_new_mem_buf(start, (int)(finish - start));
        cert = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
        BIO_free(bio);
        if (!cert) {
            fail(s, "invalid certificate in %s", path);
            goto done;
        }
        ok = handle_certificate(s, cert, kind);
        X509_free(cert);
        if (!ok) goto done;
        ++count;
        cursor = finish;
    }
    if (!count && (!allow_empty || strspn(data, " \t\r\n") != (size_t)length)) {
        fail(s, "no certificates in %s", path);
        goto done;
    }
    if (parsed) *parsed = count;
    ok = 1;
done:
    if (s->error_size && s->error[0]) ok = 0;
    free(data);
    fclose(file);
    return ok;
}

static int certificate_name(const struct dirent *entry)
{
    const char *p = entry->d_name;
    int i;
    if (strlen(p) < 10) return 0;
    for (i = 0; i < 8; ++i)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
    if (p[8] != '.' || !p[9]) return 0;
    for (p += 9; *p; ++p) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static void remove_temporary(const char *directory)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    if (dir) {
        while ((entry = readdir(dir))) {
            char *path;
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            path = path_join(directory, entry->d_name);
            if (path) {
                unlink(path);
                free(path);
            }
        }
        closedir(dir);
    }
    rmdir(directory);
}

static int files_equal(const char *a, const char *b)
{
    unsigned char left[4096], right[4096];
    struct stat st;
    int fa = open(a, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int fb = open(b, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int ok = 0;
    if (fa < 0 || fb < 0 || fstat(fb, &st) || !S_ISREG(st.st_mode)) goto done;
    for (;;) {
        ssize_t na = read(fa, left, sizeof(left));
        ssize_t nb = read(fb, right, sizeof(right));
        if (na < 0 || nb < 0 || na != nb || memcmp(left, right, (size_t)na)) break;
        if (!na) {
            ok = 1;
            break;
        }
    }
done:
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    return ok;
}

static int snapshots_equal(const char *temporary, const char *existing, size_t expected)
{
    DIR *dir = opendir(existing);
    struct dirent *entry;
    size_t count = 0;
    int ok = 1;
    if (!dir) return 0;
    while ((entry = readdir(dir))) {
        char *a, *b;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!certificate_name(entry)) {
            ok = 0;
            break;
        }
        a = path_join(temporary, entry->d_name);
        b = path_join(existing, entry->d_name);
        if (!a || !b || !files_equal(a, b)) ok = 0;
        free(a);
        free(b);
        if (!ok) break;
        ++count;
    }
    closedir(dir);
    return ok && count == expected;
}

static int export_blacklist(store *s, const char *path)
{
    char *const argv[] = { "/usr/bin/trust", "extract", "--filter=blacklist",
                           "--format=pem-bundle", (char *)path, NULL };
    pid_t pid;
    int status, result = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);
    if (result) {
        fail(s, "cannot start p11-kit trust: %s", strerror(result));
        return 0;
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        fail(s, "cannot wait for p11-kit trust: %s", strerror(errno));
        return 0;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fail(s, "p11-kit could not export the certificate blacklist");
        return 0;
    }
    return 1;
}

static int export_policy(store *s, const char *path)
{
    char *const argv[] = { "/usr/bin/trust", "dump", NULL };
    posix_spawn_file_actions_t actions;
    int fd, result, status;
    pid_t pid;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        fail(s, "cannot create p11-kit policy snapshot: %s", strerror(errno));
        return 0;
    }
    result = posix_spawn_file_actions_init(&actions);
    if (!result) {
        result = posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
        if (!result) result = posix_spawn_file_actions_addclose(&actions, fd);
        if (!result) result = posix_spawn(&pid, argv[0], &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(fd);
    if (result) {
        fail(s, "cannot read p11-kit policy: %s", strerror(result));
        return 0;
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        fail(s, "cannot wait for p11-kit policy: %s", strerror(errno));
        return 0;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fail(s, "p11-kit could not export certificate policy");
        return 0;
    }
    return 1;
}

static int read_restricted(store *s, const char *path)
{
    static const char object[] = "[p11-kit-object-v1]";
    static const char attribute[] = "nss-server-distrust-after: ";
    FILE *file = fopen(path, "rb");
    char *data = NULL, *cursor;
    long length;
    int ok = 0;
    if (!file) {
        fail(s, "cannot read p11-kit policy %s: %s", path, strerror(errno));
        return 0;
    }
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || length > 16 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET) || !(data = malloc((size_t)length + 1)) ||
        fread(data, 1, (size_t)length, file) != (size_t)length) {
        fail(s, "cannot read p11-kit policy %s", path);
        goto done;
    }
    data[length] = 0;
    cursor = data;
    while ((cursor = strstr(cursor, object))) {
        char *next = strstr(cursor + sizeof(object) - 1, object);
        char *attr = strstr(cursor, attribute);
        if (attr && (!next || attr < next)) {
            char *value = attr + sizeof(attribute) - 1;
            if (strncmp(value, "\"%00\"", 5)) {
                char *start = strstr(value, "-----BEGIN CERTIFICATE-----");
                char *end = start ? strstr(start, "-----END CERTIFICATE-----") : NULL;
                BIO *bio;
                X509 *cert;
                unsigned char digest[SHA256_DIGEST_LENGTH];
                unsigned int digest_length = 0;
                if (!start || !end || (next && end >= next)) {
                    fail(s, "restricted p11-kit certificate cannot be exported");
                    goto done;
                }
                end += strlen("-----END CERTIFICATE-----");
                bio = BIO_new_mem_buf(start, (int)(end - start));
                cert = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
                BIO_free(bio);
                if (!cert || !X509_digest(cert, EVP_sha256(), digest, &digest_length) ||
                    digest_length != sizeof(digest) || !add_fingerprint(&s->restricted, digest)) {
                    X509_free(cert);
                    fail(s, "cannot read restricted p11-kit certificate");
                    goto done;
                }
                X509_free(cert);
            }
        }
        if (!next) break;
        cursor = next;
    }
    ok = 1;
done:
    free(data);
    fclose(file);
    return ok;
}

static int count_blacklist_objects(store *s, size_t *count)
{
    char *const argv[] = { "/usr/bin/trust", "list", "--filter=blacklist", NULL };
    posix_spawn_file_actions_t actions;
    int fds[2], result, status, read_error;
    pid_t pid;
    FILE *stream;
    char *line = NULL;
    size_t capacity = 0;
    *count = 0;
    if (pipe2(fds, O_CLOEXEC)) {
        fail(s, "cannot inspect p11-kit blacklist: %s", strerror(errno));
        return 0;
    }
    result = posix_spawn_file_actions_init(&actions);
    if (!result) {
        result = posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        if (!result) result = posix_spawn_file_actions_addclose(&actions, fds[0]);
        if (!result) result = posix_spawn_file_actions_addclose(&actions, fds[1]);
        if (!result) result = posix_spawn(&pid, argv[0], &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(fds[1]);
    if (result) {
        close(fds[0]);
        fail(s, "cannot list p11-kit blacklist: %s", strerror(result));
        return 0;
    }
    stream = fdopen(fds[0], "r");
    if (!stream) {
        close(fds[0]);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        fail(s, "cannot read p11-kit blacklist");
        return 0;
    }
    while (getline(&line, &capacity, stream) >= 0)
        if (!strncmp(line, "pkcs11:", 7)) ++*count;
    read_error = ferror(stream);
    free(line);
    fclose(stream);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        fail(s, "cannot wait for p11-kit blacklist: %s", strerror(errno));
        return 0;
    }
    if (read_error || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fail(s, "p11-kit could not list the certificate blacklist");
        return 0;
    }
    return 1;
}

char *acl_system_cacerts_prepare(const char *android_root, const char *data_dir,
                                 const char *bundle, const char *blacklist, const char *policy,
                                 int *system_available, size_t *cert_count,
                                 size_t *excluded_count, char *error, size_t error_size)
{
    static const unsigned char distrusted[][SHA256_DIGEST_LENGTH] = {
        { 0x86,0xa1,0xec,0xba,0x08,0x9c,0x4a,0x8d,0x3b,0xbe,0x27,0x34,0xc6,0x12,0xba,0x34,
          0x1d,0x81,0x3e,0x04,0x3c,0xf9,0xe8,0xa8,0x62,0xcd,0x5c,0x57,0xa3,0x6b,0xbe,0x6b }
    };
    const char *system_bundle = bundle;
    struct stat st;
    struct dirent **entries = NULL;
    char *base = NULL, *temporary = NULL, *deny_file = NULL, *policy_file = NULL, *builtins = NULL, *result = NULL;
    char digest_text[SHA256_DIGEST_LENGTH * 2 + 1];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    size_t blacklist_objects = 0, blacklist_certificates = 0;
    store s;
    int count = 0, i, ok = 0, temporary_created = 0, has_bundle = 1;
    memset(&s, 0, sizeof(s));
    s.error = error;
    s.error_size = error_size;
    if (system_available) *system_available = 0;
    if (cert_count) *cert_count = 0;
    if (excluded_count) *excluded_count = 0;
    if (error_size) error[0] = 0;
    if (!system_bundle) {
        system_bundle = "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem";
        if (stat(system_bundle, &st) && errno == ENOENT)
            system_bundle = "/etc/pki/tls/certs/ca-bundle.crt";
    }
    if (stat(system_bundle, &st)) {
        /* Sailjail hides /etc/pki from apps without the Internet permission; they use the built-in store. */
        if (errno != ENOENT && errno != EACCES && errno != EPERM) {
            fail(&s, "cannot inspect system TLS bundle: %s", strerror(errno));
            return NULL;
        }
        has_bundle = 0;
    }
    if (has_bundle && !S_ISREG(st.st_mode)) {
        fail(&s, "system TLS bundle is not a regular file");
        return NULL;
    }
    if (system_available) *system_available = has_bundle;
    if (!has_bundle && !blacklist && access("/usr/bin/trust", X_OK)) return NULL;
    if (!data_dir || !*data_dir) {
        fail(&s, "no private data directory for system certificates");
        return NULL;
    }
    base = path_join(data_dir, "system-cacerts");
    if (!base || (mkdir(base, 0700) && errno != EEXIST)) {
        fail(&s, "cannot create certificate storage: %s", strerror(errno));
        goto done;
    }
    temporary = path_join(base, ".tmp-XXXXXX");
    if (!temporary || !mkdtemp(temporary)) {
        fail(&s, "cannot create temporary certificate storage: %s", strerror(errno));
        goto done;
    }
    temporary_created = 1;
    s.directory = temporary;
    SHA256_Init(&s.snapshot);
    for (i = 0; i < (int)(sizeof(distrusted) / sizeof(distrusted[0])); ++i)
        if (!add_fingerprint(&s.denied, distrusted[i])) {
            fail(&s, "out of memory preparing certificate blacklist");
            goto done;
        }
    deny_file = path_join(temporary, ".blacklist");
    if (!deny_file) {
        fail(&s, "out of memory creating blacklist path");
        goto done;
    }
    /* The app's sandbox may hide p11-kit; the built-in distrust list still applies. */
    if (blacklist) {
        if (!read_certificates(&s, blacklist, 1, 1, NULL)) goto done;
    } else if (!access("/usr/bin/trust", X_OK)) {
        if (!count_blacklist_objects(&s, &blacklist_objects) ||
            !export_blacklist(&s, deny_file) ||
            !read_certificates(&s, deny_file, 1, 1, &blacklist_certificates)) goto done;
        if (blacklist_objects != blacklist_certificates) {
            fail(&s, "p11-kit blacklist contains entries that cannot be exported as certificates");
            goto done;
        }
        if (unlink(deny_file)) {
            fail(&s, "cannot remove temporary blacklist: %s", strerror(errno));
            goto done;
        }
    }
    policy_file = path_join(temporary, ".policy");
    if (!policy_file) {
        fail(&s, "out of memory creating policy path");
        goto done;
    }
    if (policy) {
        if (!read_restricted(&s, policy)) goto done;
    } else if (!access("/usr/bin/trust", X_OK)) {
        if (!export_policy(&s, policy_file) || !read_restricted(&s, policy_file)) goto done;
        if (unlink(policy_file)) {
            fail(&s, "cannot remove temporary policy: %s", strerror(errno));
            goto done;
        }
    }
    builtins = path_join(android_root, "etc/security/cacerts");
    if (!builtins || (count = scandir(builtins, &entries, certificate_name, alphasort)) < 0) {
        fail(&s, "cannot read built-in certificates: %s", strerror(errno));
        goto done;
    }
    for (i = 0; i < count; ++i) {
        char *path = path_join(builtins, entries[i]->d_name);
        if (!path || !read_certificates(&s, path, 0, 0, NULL)) {
            if (!path) fail(&s, "out of memory reading built-in certificates");
            free(path);
            goto done;
        }
        free(path);
    }
    if (has_bundle && !read_certificates(&s, system_bundle, 2, 0, NULL)) goto done;
    if (!s.written) {
        fail(&s, "merged certificate store is empty");
        goto done;
    }
    SHA256_Final(digest, &s.snapshot);
    for (i = 0; i < SHA256_DIGEST_LENGTH; ++i) sprintf(digest_text + 2 * i, "%02x", digest[i]);
    result = path_join(base, digest_text);
    if (!result) {
        fail(&s, "out of memory creating certificate snapshot");
        goto done;
    }
    if (rename(temporary, result)) {
        if ((errno != EEXIST && errno != ENOTEMPTY) || lstat(result, &st) || !S_ISDIR(st.st_mode)) {
            fail(&s, "cannot publish certificate snapshot: %s", strerror(errno));
            free(result);
            result = NULL;
            goto done;
        }
        if (!snapshots_equal(temporary, result, s.written)) {
            fail(&s, "existing certificate snapshot does not match its contents");
            free(result);
            result = NULL;
            goto done;
        }
        remove_temporary(temporary);
    }
    if (cert_count) *cert_count = s.written;
    if (excluded_count) *excluded_count = s.excluded;
    ok = 1;
done:
    if (!ok && temporary_created) remove_temporary(temporary);
    for (i = 0; i < count; ++i) free(entries[i]);
    free(entries);
    free(s.denied.values);
    free(s.restricted.values);
    free(s.seen.values);
    free(base);
    free(temporary);
    free(deny_file);
    free(policy_file);
    free(builtins);
    return result;
}
