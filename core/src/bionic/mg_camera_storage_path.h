#ifndef MG_CAMERA_STORAGE_PATH_H
#define MG_CAMERA_STORAGE_PATH_H

#include <stddef.h>

static inline const char *mg_camera_path_component(const char *path, const char *component)
{
    while (*component) {
        unsigned char ch = (unsigned char)*path++;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != (unsigned char)*component++) return NULL;
    }
    return *path == 0 || *path == '/' ? path : NULL;
}

/* Receives an absolute, normalized Android shared-storage path. */
static inline const char *mg_camera_storage_suffix(const char *path)
{
    static const char root[] = "/storage/emulated/0/";
    size_t i;
    const char *suffix;
    for (i = 0; i < sizeof(root) - 1; ++i)
        if (path[i] != root[i]) return NULL;
    path += sizeof(root) - 1;
    suffix = mg_camera_path_component(path, "dcim");
    if (suffix) return suffix;
    suffix = mg_camera_path_component(path, "pictures");
    return suffix && *suffix == '/' ? mg_camera_path_component(suffix + 1, "dcim") : NULL;
}

#endif
