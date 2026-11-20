#ifndef GPU_PROBE_H
#define GPU_PROBE_H

#include <QString>

/*
 * What this device offers for getting a graphics-processor frame out of one Aurora process
 * and into another (aurora/embedded_gpu_transport.cpp): which EGL extensions exist, whether
 * a buffer can be exported as a dma-buf, what the platform allocator (gralloc) is, and
 * whether its memory can still be had and shared when the (MESA) dma-buf export is not
 * there. Written for `ru.androidcompatibilitylayer.appmanager --gpu-probe`, whose output is
 * a text report: it changes nothing on the device.
 */
namespace GpuProbe {
QString report();
}

#endif
