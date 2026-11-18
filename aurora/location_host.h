#ifndef AURORA_LOCATION_HOST_H
#define AURORA_LOCATION_HOST_H

#include "../core/include/megler.h"

// Create after QCoreApplication and before starting Megler.
megler_location_host auroraLocationHost();

// Configure before starting Megler; false means the coordinates or accuracy are invalid.
bool auroraLocationSetFixedLocation(double latitude, double longitude, float accuracyMeters = 0.0f);
void auroraLocationUseDeviceLocation();

// Call before destroying QCoreApplication, not from a location callback.
void auroraLocationShutdown();

#endif
