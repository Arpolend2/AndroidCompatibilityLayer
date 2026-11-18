#pragma once

#include "../core/include/megler.h"

// Create after QCoreApplication and before starting Megler.
megler_sensor_host auroraSensorHost();

// Call before destroying QCoreApplication, not from a sensor callback.
// The host cannot be restarted; callbacks obtained earlier remain safe.
void auroraSensorShutdown();
