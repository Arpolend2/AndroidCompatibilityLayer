#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_STORE_WORKER_TEARDOWN_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_STORE_WORKER_TEARDOWN_H

#include <cstdio>
#include <unistd.h>

inline void traceStoreWorkerTeardown(const char *stage)
{
    // Avoid the Qt log handler and the stdio stream lock during teardown.
    char line[160];
    const int length = std::snprintf(line, sizeof(line), "Aurora Store worker: teardown: pid=%ld %s\n",
                                     static_cast<long>(::getpid()), stage);
    if (length > 0 && length < int(sizeof(line))) (void)::write(STDERR_FILENO, line, size_t(length));
}

#endif
