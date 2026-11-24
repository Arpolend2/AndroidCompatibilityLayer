#ifndef ANDROID_COMPATIBILITY_LAYER_CODEC_MPP_ABI_H
#define ANDROID_COMPATIBILITY_LAYER_CODEC_MPP_ABI_H

#include <cstdint>

// Public Rockchip MPP ABI: rk_mpi.h, rk_mpi_cmd.h and rk_type.h.
struct AclMppApi {
    uint32_t size;
    uint32_t version;
    int (*decode)(void *, void *, void **);
    int (*decode_put_packet)(void *, void *);
    int (*decode_get_frame)(void *, void **);
    int (*encode)(void *, void *, void **);
    int (*encode_put_frame)(void *, void *);
    int (*encode_get_packet)(void *, void **);
    int (*isp)(void *, void *, void *);
    int (*isp_put_frame)(void *, void *);
    int (*isp_get_frame)(void *, void **);
    int (*poll)(void *, int, int);
    int (*dequeue)(void *, int, void **);
    int (*enqueue)(void *, int, void *);
    int (*reset)(void *);
    int (*control)(void *, int, void *);
};

enum {
    ACL_MPP_DEC = 0,
    ACL_MPP_AVC = 7,
    ACL_MPP_HEVC = 0x01000004,
    ACL_MPP_OUTPUT_BLOCK = 0x00200004,
    ACL_MPP_OUTPUT_TIMEOUT = 0x00200005,
    ACL_MPP_INFO_READY = 0x00310003,
    ACL_MPP_TIMEOUT = -8,
    ACL_MPP_BUFFER_FULL = -1012
};

#endif
