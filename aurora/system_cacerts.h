#ifndef ACL_SYSTEM_CACERTS_H
#define ACL_SYSTEM_CACERTS_H

#include <stddef.h>

/* A null result with an empty error means that the host has no system bundle. */
char *acl_system_cacerts_prepare(const char *android_root, const char *data_dir,
                                 const char *bundle, const char *blacklist, const char *policy,
                                 int *system_available, size_t *cert_count,
                                 size_t *excluded_count, char *error, size_t error_size);

#endif
