#include "location_host.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QGeoCoordinate>
#include <QGeoPositionInfo>
#include <QGeoPositionInfoSource>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <cmath>
#include <limits>
#include <mutex>

#ifdef MEGLER_LOCATION_HOST_TESTING
#include "location_host_test_support.h"
#endif

namespace {

#ifdef MEGLER_LOCATION_HOST_TESTING
AuroraLocationTestSourceFactory g_testSourceFactory = nullptr;
#endif

bool toFloat(qreal value, float *result)
{
    if (!std::isfinite(value) || value < 0 ||
        value > static_cast<qreal>(std::numeric_limits<float>::max())) return false;
    *result = static_cast<float>(value);
    return true;
}

bool positionToFix(const QGeoPositionInfo &position, int source, megler_location *fix)
{
    const QGeoCoordinate coordinate = position.coordinate();
    if (!coordinate.isValid() || !position.timestamp().isValid()) return false;

    megler_location result = {};
    result.latitude = coordinate.latitude();
    result.longitude = coordinate.longitude();
    result.time_ms = position.timestamp().toMSecsSinceEpoch();
    result.source = source;

    if (std::isfinite(coordinate.altitude())) {
        result.altitude = coordinate.altitude();
        result.has |= MEGLER_LOCATION_HAS_ALTITUDE;
    }
    if (position.hasAttribute(QGeoPositionInfo::HorizontalAccuracy)) {
        const qreal value = position.attribute(QGeoPositionInfo::HorizontalAccuracy);
        toFloat(value, &result.accuracy_m);
    }
    if (position.hasAttribute(QGeoPositionInfo::VerticalAccuracy)) {
        const qreal value = position.attribute(QGeoPositionInfo::VerticalAccuracy);
        if (toFloat(value, &result.altitude_accuracy_m)) {
            result.has |= MEGLER_LOCATION_HAS_ALTITUDE_ACCURACY;
        }
    }
    if (position.hasAttribute(QGeoPositionInfo::GroundSpeed)) {
        const qreal value = position.attribute(QGeoPositionInfo::GroundSpeed);
        if (toFloat(value, &result.speed_mps)) {
            result.has |= MEGLER_LOCATION_HAS_SPEED;
        }
    }
    if (position.hasAttribute(QGeoPositionInfo::Direction)) {
        const qreal value = position.attribute(QGeoPositionInfo::Direction);
        if (value <= 360 && toFloat(value, &result.bearing_deg)) {
            result.has |= MEGLER_LOCATION_HAS_BEARING;
        }
    }
    *fix = result;
    return true;
}

class Worker : public QObject {
    Q_OBJECT
public:
    int access() const
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_fixed) return MEGLER_LOCATION_ON;
        if (!m_source || m_sourceUnavailable) return MEGLER_LOCATION_NONE;
        if (m_accessDenied) return MEGLER_LOCATION_DENIED;
        // LocationService starts updates only after access() returns ON.
        return MEGLER_LOCATION_ON;
    }

    bool last(megler_location *fix) const
    {
        if (!fix) return false;
        QMutexLocker lock(&m_stateMutex);
        if (m_fixed) {
            *fix = {};
            fix->latitude = m_fixedLatitude;
            fix->longitude = m_fixedLongitude;
            fix->accuracy_m = m_fixedAccuracy;
            fix->time_ms = QDateTime::currentMSecsSinceEpoch();
            fix->source = MEGLER_LOCATION_SET_BY_HAND;
            return true;
        }
        if (!m_hasRealFix) return false;
        *fix = m_realFix;
        return true;
    }

public slots:
    void initialize()
    {
#ifdef MEGLER_LOCATION_HOST_TESTING
        if (g_testSourceFactory) {
            m_source = g_testSourceFactory();
            if (m_source) m_source->setParent(this);
        }
#endif
        if (!m_source) m_source = QGeoPositionInfoSource::createDefaultSource(this);
        if (!m_source) {
            updateSourceError(QGeoPositionInfoSource::UnknownSourceError);
            qWarning() << "location: no Qt Positioning source is available";
            return;
        }

        connect(m_source, &QGeoPositionInfoSource::positionUpdated, this,
                [this](const QGeoPositionInfo &position) { acceptPosition(position); });
        typedef void (QGeoPositionInfoSource::*SourceErrorSignal)(QGeoPositionInfoSource::Error);
        connect(m_source, static_cast<SourceErrorSignal>(&QGeoPositionInfoSource::error), this,
                [this](QGeoPositionInfoSource::Error error) { updateSourceError(error); });
        connect(m_source, &QGeoPositionInfoSource::updateTimeout, this,
                []() { qWarning() << "location: Qt Positioning update timed out"; });

        updateSourceError(m_source->error());
        const QGeoPositionInfoSource::PositioningMethods methods = m_source->supportedPositioningMethods();
        if (methods == QGeoPositionInfoSource::NoPositioningMethods)
            updateSourceError(QGeoPositionInfoSource::UnknownSourceError);

        qInfo() << "location: Qt Positioning source"
                << (methods == QGeoPositionInfoSource::NoPositioningMethods ? "has no methods" : "is available");
    }

    bool start(int intervalMs)
    {
        if (intervalMs < 0) {
            qWarning() << "location: invalid update interval" << intervalMs;
            return false;
        }
        m_requested = true;
        m_intervalMs = intervalMs;
        if (isFixed()) return true;
        return startSource();
    }

    void stop()
    {
        m_requested = false;
        if (m_sourceRunning && m_source) m_source->stopUpdates();
        m_sourceRunning = false;
    }

    bool setFixedLocation(double latitude, double longitude, float accuracyMeters)
    {
        if (!std::isfinite(latitude) || !std::isfinite(longitude) ||
            latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0 ||
            !std::isfinite(accuracyMeters) || accuracyMeters < 0.0f) {
            qWarning() << "location: invalid fixed coordinates or accuracy";
            return false;
        }

        if (m_sourceRunning && m_source) {
            m_source->stopUpdates();
            m_sourceRunning = false;
        }
        {
            QMutexLocker lock(&m_stateMutex);
            m_fixed = true;
            m_fixedLatitude = latitude;
            m_fixedLongitude = longitude;
            m_fixedAccuracy = accuracyMeters;
        }
        qInfo() << "location: using user-set coordinates";
        return true;
    }

    void useDeviceLocation()
    {
        {
            QMutexLocker lock(&m_stateMutex);
            m_fixed = false;
        }
        qInfo() << "location: using the device location source";
        if (m_requested) startSource();
    }

    void cleanup()
    {
        stop();
        delete m_source;
        m_source = nullptr;
        updateSourceError(QGeoPositionInfoSource::UnknownSourceError);
    }

private:
    bool isFixed() const
    {
        QMutexLocker lock(&m_stateMutex);
        return m_fixed;
    }

    bool startSource()
    {
        if (!m_source) return false;
        {
            QMutexLocker lock(&m_stateMutex);
            if (m_sourceUnavailable) return false;
        }
        const int interval = m_intervalMs == 0
            ? 0 : qMax(m_intervalMs, m_source->minimumUpdateInterval());
        m_source->setUpdateInterval(interval);
        m_source->startUpdates();
        const QGeoPositionInfoSource::Error error = m_source->error();
        updateSourceError(error);
        m_sourceRunning = error == QGeoPositionInfoSource::NoError;
        if (!m_sourceRunning)
            qWarning() << "location: could not start Qt Positioning updates; error" << error;
        else
            qInfo() << "location: following the device position every" << m_intervalMs << "ms";
        return m_sourceRunning;
    }

    void acceptPosition(const QGeoPositionInfo &position)
    {
        if (!position.isValid()) {
            qWarning() << "location: ignored an invalid position update";
            return;
        }
        const QGeoPositionInfoSource::PositioningMethods methods = m_source
            ? m_source->supportedPositioningMethods() : QGeoPositionInfoSource::NoPositioningMethods;
        const bool satellitesOnly =
            (methods & QGeoPositionInfoSource::SatellitePositioningMethods) &&
            !(methods & QGeoPositionInfoSource::NonSatellitePositioningMethods);
        megler_location fix = {};
        if (!positionToFix(position, satellitesOnly ? MEGLER_LOCATION_SATELLITES : MEGLER_LOCATION_NETWORK, &fix)) {
            qWarning() << "location: ignored a position without valid coordinates or timestamp";
            return;
        }
        bool firstOrRecovery = false;
        {
            QMutexLocker lock(&m_stateMutex);
            firstOrRecovery = !m_hasRealFix || m_accessDenied || m_sourceUnavailable;
            m_realFix = fix;
            m_hasRealFix = true;
            m_accessDenied = false;
            m_sourceUnavailable = false;
        }
        if (firstOrRecovery) qInfo() << "location: received a device position fix";
    }

    void updateSourceError(QGeoPositionInfoSource::Error error)
    {
        QMutexLocker lock(&m_stateMutex);
        m_accessDenied = error == QGeoPositionInfoSource::AccessError;
        m_sourceUnavailable = error == QGeoPositionInfoSource::UnknownSourceError;
        if (error != QGeoPositionInfoSource::NoError) m_hasRealFix = false;
        if (error != m_lastError && error != QGeoPositionInfoSource::NoError)
            qWarning() << "location: Qt Positioning reported error" << error;
        else if (error == QGeoPositionInfoSource::NoError &&
                 m_lastError != QGeoPositionInfoSource::NoError)
            qInfo() << "location: Qt Positioning recovered";
        m_lastError = error;
    }

    QGeoPositionInfoSource *m_source = nullptr;
    mutable QMutex m_stateMutex;
    megler_location m_realFix = {};
    bool m_hasRealFix = false;
    bool m_fixed = false;
    double m_fixedLatitude = 0;
    double m_fixedLongitude = 0;
    float m_fixedAccuracy = 0;
    bool m_accessDenied = false;
    bool m_sourceUnavailable = false;
    QGeoPositionInfoSource::Error m_lastError = QGeoPositionInfoSource::NoError;
    bool m_requested = false;
    bool m_sourceRunning = false;
    int m_intervalMs = 0;
};

class Host {
public:
    explicit Host()
    {
        if (!QCoreApplication::instance()) qFatal("location: QCoreApplication is required");
        m_worker.moveToThread(&m_thread);
        m_thread.start();
        if (!QMetaObject::invokeMethod(&m_worker, "initialize", Qt::BlockingQueuedConnection))
            qFatal("location: cannot initialize Qt Positioning");
    }

    int access(bool ask)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) return MEGLER_LOCATION_NONE;
        if (ask && !m_saidAsk) {
            qInfo() << "location: access is controlled by Aurora's host-process Location permission; no runtime prompt is available";
            m_saidAsk = true;
        }
        return m_worker.access();
    }

    bool start(int intervalMs)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) return false;
        bool result = false;
        const bool invoked = QMetaObject::invokeMethod(
            &m_worker, "start", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(bool, result), Q_ARG(int, intervalMs));
        if (!invoked) qFatal("location: cannot start position updates");
        return result;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) return;
        if (!QMetaObject::invokeMethod(&m_worker, "stop", Qt::BlockingQueuedConnection))
            qFatal("location: cannot stop position updates");
    }

    bool setFixedLocation(double latitude, double longitude, float accuracyMeters)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) return false;
        bool result = false;
        const bool invoked = QMetaObject::invokeMethod(
            &m_worker, "setFixedLocation", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(bool, result), Q_ARG(double, latitude),
            Q_ARG(double, longitude), Q_ARG(float, accuracyMeters));
        if (!invoked) qFatal("location: cannot set fixed coordinates");
        return result;
    }

    void useDeviceLocation()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) return;
        if (!QMetaObject::invokeMethod(&m_worker, "useDeviceLocation", Qt::BlockingQueuedConnection))
            qFatal("location: cannot restore the device location source");
    }

    bool last(megler_location *fix)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_closed && m_worker.last(fix);
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) return;
        m_closed = true;
        if (!QMetaObject::invokeMethod(&m_worker, "cleanup", Qt::BlockingQueuedConnection))
            qFatal("location: cannot shut down Qt Positioning");
        m_thread.quit();
        m_thread.wait();
    }

private:
    std::mutex m_mutex;
    bool m_closed = false;
    bool m_saidAsk = false;
    QThread m_thread;
    Worker m_worker;
};

Host &host()
{
    // The core may retain callback pointers after shutdown.
    static Host *instance = new Host;
    return *instance;
}

int MEGLER_CB accessThunk(void *user, bool ask)
{
    return static_cast<Host *>(user)->access(ask);
}

bool MEGLER_CB startThunk(void *user, int intervalMs)
{
    return static_cast<Host *>(user)->start(intervalMs);
}

void MEGLER_CB stopThunk(void *user)
{
    static_cast<Host *>(user)->stop();
}

bool MEGLER_CB lastThunk(void *user, megler_location *fix)
{
    return static_cast<Host *>(user)->last(fix);
}

}

#ifdef MEGLER_LOCATION_HOST_TESTING
void auroraLocationSetTestSourceFactory(AuroraLocationTestSourceFactory factory)
{
    g_testSourceFactory = factory;
}
#endif

megler_location_host auroraLocationHost()
{
    megler_location_host result = {};
    result.user = &host();
    result.access = accessThunk;
    result.start = startThunk;
    result.stop = stopThunk;
    result.last = lastThunk;
    return result;
}

bool auroraLocationSetFixedLocation(double latitude, double longitude, float accuracyMeters)
{
    return host().setFixedLocation(latitude, longitude, accuracyMeters);
}

void auroraLocationUseDeviceLocation()
{
    host().useDeviceLocation();
}

void auroraLocationShutdown()
{
    host().shutdown();
}

#include "location_host.moc"
