#include "../location_host.h"
#include "../location_host_test_support.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QGeoCoordinate>
#include <QGeoPositionInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>
#include <cassert>
#include <cmath>

namespace {

std::atomic<int> starts(0);
std::atomic<int> stops(0);

class FakeSource : public QGeoPositionInfoSource {
    Q_OBJECT
public:
    FakeSource() : QGeoPositionInfoSource(nullptr) {}

    QGeoPositionInfo lastKnownPosition(bool = false) const override { return {}; }
    int minimumUpdateInterval() const override { return 100; }
    PositioningMethods supportedPositioningMethods() const override { return AllPositioningMethods; }
    Error error() const override { return static_cast<Error>(m_error.load()); }
    void startUpdates() override { ++starts; }
    void stopUpdates() override { ++stops; }
    void requestUpdate(int = 0) override {}

    void setPendingPosition(const QGeoPositionInfo &position)
    {
        QMutexLocker lock(&m_mutex);
        m_pendingPosition = position;
    }

public slots:
    void publishPending()
    {
        QGeoPositionInfo position;
        {
            QMutexLocker lock(&m_mutex);
            position = m_pendingPosition;
        }
        emit positionUpdated(position);
    }

    void failAccess()
    {
        m_error.store(AccessError);
        emit QGeoPositionInfoSource::error(AccessError);
    }

private:
    std::atomic<int> m_error{NoError};
    QMutex m_mutex;
    QGeoPositionInfo m_pendingPosition;
};

std::atomic<FakeSource *> source(nullptr);

QGeoPositionInfoSource *createFakeSource()
{
    FakeSource *created = new FakeSource;
    source.store(created);
    return created;
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    auroraLocationSetTestSourceFactory(createFakeSource);
    const megler_location_host host = auroraLocationHost();

    assert(host.access(host.user, false) == MEGLER_LOCATION_ON);
    assert(!host.start(host.user, -1));
    assert(host.start(host.user, 1000));
    assert(starts.load() == 1);

    const qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    QGeoPositionInfo position(QGeoCoordinate(55.751244, 37.618423, 144.0),
                              QDateTime::fromMSecsSinceEpoch(timestamp, Qt::UTC));
    position.setAttribute(QGeoPositionInfo::HorizontalAccuracy, 12.5);
    position.setAttribute(QGeoPositionInfo::VerticalAccuracy, 8.0);
    position.setAttribute(QGeoPositionInfo::GroundSpeed, 1.5);
    position.setAttribute(QGeoPositionInfo::Direction, 90.0);
    FakeSource *fake = source.load();
    assert(fake);
    fake->setPendingPosition(position);
    assert(QMetaObject::invokeMethod(fake, "publishPending", Qt::BlockingQueuedConnection));

    megler_location fix = {};
    assert(host.last(host.user, &fix));
    assert(std::abs(fix.latitude - 55.751244) < 0.000001);
    assert(std::abs(fix.longitude - 37.618423) < 0.000001);
    assert(fix.altitude == 144.0);
    assert(fix.accuracy_m == 12.5f);
    assert(fix.altitude_accuracy_m == 8.0f);
    assert(fix.speed_mps == 1.5f);
    assert(fix.bearing_deg == 90.0f);
    assert(fix.has == (MEGLER_LOCATION_HAS_ALTITUDE | MEGLER_LOCATION_HAS_ALTITUDE_ACCURACY |
                       MEGLER_LOCATION_HAS_SPEED | MEGLER_LOCATION_HAS_BEARING));
    assert(fix.source == MEGLER_LOCATION_NETWORK);
    assert(fix.time_ms == timestamp);

    assert(auroraLocationSetFixedLocation(-90.0, -180.0));
    assert(host.last(host.user, &fix));
    assert(fix.latitude == -90.0 && fix.longitude == -180.0);
    assert(fix.accuracy_m == 0.0f);
    assert(fix.source == MEGLER_LOCATION_SET_BY_HAND);
    assert(stops.load() == 1);
    auroraLocationUseDeviceLocation();
    assert(starts.load() == 2);

    assert(QMetaObject::invokeMethod(fake, "failAccess", Qt::BlockingQueuedConnection));
    assert(host.access(host.user, false) == MEGLER_LOCATION_DENIED);
    assert(!host.last(host.user, &fix));
    host.stop(host.user);
    auroraLocationShutdown();
    auroraLocationShutdown();
    assert(host.access(host.user, false) == MEGLER_LOCATION_NONE);
    assert(!host.start(host.user, 1000));
    assert(!host.last(host.user, &fix));
    assert(!auroraLocationSetFixedLocation(0.0, 0.0));
}

#include "location_host_test.moc"
