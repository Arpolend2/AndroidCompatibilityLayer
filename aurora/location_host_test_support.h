#ifndef AURORA_LOCATION_HOST_TEST_SUPPORT_H
#define AURORA_LOCATION_HOST_TEST_SUPPORT_H

#include <QGeoPositionInfoSource>

typedef QGeoPositionInfoSource *(*AuroraLocationTestSourceFactory)();

void auroraLocationSetTestSourceFactory(AuroraLocationTestSourceFactory factory);

#endif
