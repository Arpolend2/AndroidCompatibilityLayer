#include "sensor_host.h"

#include <QAccelerometer>
#include <QCoreApplication>
#include <QDebug>
#include <QGyroscope>
#include <QLightSensor>
#include <QMagnetometer>
#include <QMetaObject>
#include <QPressureSensor>
#include <QProximitySensor>
#include <QSensor>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace {

const double gravity = 9.80665;
const int lastKind = MEGLER_SENSOR_BAROMETER;

int rateForInterval(int interval, const qrangelist &ranges)
{
    const int wanted = interval <= 0 ? std::numeric_limits<int>::max()
                                     : std::max(1, static_cast<int>((1000LL + interval - 1) / interval));
    int fastest = 0;
    int best = std::numeric_limits<int>::max();
    for (const qrange &range : ranges) {
        if (range.second <= 0 || range.first > range.second) continue;
        fastest = std::max(fastest, range.second);
        if (range.second >= wanted) best = std::min(best, std::max(wanted, range.first));
    }
    if (best != std::numeric_limits<int>::max()) return best;
    if (fastest) return fastest;
    return interval <= 0 ? 0 : wanted;
}

int minimumInterval(const qrangelist &ranges)
{
    int fastest = 0;
    for (const qrange &range : ranges)
        if (range.first <= range.second) fastest = std::max(fastest, range.second);
    return fastest > 0 ? static_cast<int>((1000LL + fastest - 1) / fastest) : 0;
}

megler_sensor_sample sample(int kind, double x, double y = 0, double z = 0)
{
    megler_sensor_sample result = {};
    result.kind = kind;
    result.accuracy = MEGLER_SENSOR_ACCURACY_UNKNOWN;
    if (kind == MEGLER_SENSOR_ACCELEROMETER || kind == MEGLER_SENSOR_GRAVITY ||
        kind == MEGLER_SENSOR_LINEAR_ACCELERATION) {
        x /= -gravity; y /= -gravity; z /= -gravity;
    } else if (kind == MEGLER_SENSOR_MAGNETOMETER) {
        x *= 1000000; y *= 1000000; z *= 1000000;
    } else if (kind == MEGLER_SENSOR_BAROMETER) {
        x /= 100;
    } else if (kind == MEGLER_SENSOR_PROXIMITY) {
        y = -1;
    }
    result.values[0] = x;
    result.values[1] = y;
    result.values[2] = z;
    // Qt's timestamp epoch is unspecified; zero asks the core to timestamp now.
    return result;
}

struct Device {
    QSensor *sensor = nullptr;
    bool running = false;
    int rate = -1;
};

class Worker : public QObject {
    Q_OBJECT
public:
    std::vector<megler_sensor_info> info;
    Device devices[lastKind + 1];

public slots:
    void discover()
    {
        add(MEGLER_SENSOR_ACCELEROMETER, new QAccelerometer);
        add(MEGLER_SENSOR_GRAVITY, new QAccelerometer);
        add(MEGLER_SENSOR_LINEAR_ACCELERATION, new QAccelerometer);
        add(MEGLER_SENSOR_GYROMETER, new QGyroscope);
        add(MEGLER_SENSOR_MAGNETOMETER, new QMagnetometer);
        add(MEGLER_SENSOR_LIGHT, new QLightSensor);
        add(MEGLER_SENSOR_PROXIMITY, new QProximitySensor);
        add(MEGLER_SENSOR_BAROMETER, new QPressureSensor);
        qInfo() << "sensors: discovered" << info.size() << "supported sensors";
    }

    void start(int kind, int interval)
    {
        if (interval < 0) {
            qWarning() << "sensors: invalid interval" << interval << "for kind" << kind;
            return;
        }
        if (kind < 1 || kind > lastKind || !devices[kind].sensor) {
            qWarning() << "sensors: unsupported kind" << kind;
            return;
        }
        Device &device = devices[kind];
        QSensor *sensor = device.sensor;
        const int rate = rateForInterval(interval, sensor->availableDataRates());
        if (device.running && device.rate == rate) return;
        if (device.running) {
            device.running = false;
            sensor->stop();
        }
        sensor->setDataRate(rate);
        device.rate = rate;
        device.running = true;
        if (!sensor->start()) {
            device.running = false;
            qWarning() << "sensors: start failed" << kind << "error" << sensor->error()
                       << "busy" << sensor->isBusy();
        }
    }

    void stop(int kind)
    {
        if (kind < 1 || kind > lastKind || !devices[kind].sensor) {
            qWarning() << "sensors: cannot stop unsupported kind" << kind;
            return;
        }
        Device &device = devices[kind];
        device.running = false;
        device.sensor->stop();
        device.rate = -1;
    }

    void cleanup()
    {
        for (int kind = 1; kind <= lastKind; ++kind) {
            devices[kind].running = false;
            delete devices[kind].sensor;
            devices[kind].sensor = nullptr;
        }
    }

private:
    void add(int kind, QSensor *sensor)
    {
        sensor->setParent(this);
        if (!sensor->connectToBackend()) {
            qInfo() << "sensors: no backend for kind" << kind;
            delete sensor;
            return;
        }
        if (kind == MEGLER_SENSOR_GRAVITY || kind == MEGLER_SENSOR_LINEAR_ACCELERATION) {
            QAccelerometer *accel = static_cast<QAccelerometer *>(sensor);
            if (!sensor->isFeatureSupported(QSensor::AccelerationMode)) {
                qInfo() << "sensors: acceleration mode unsupported for kind" << kind;
                delete sensor;
                return;
            }
            const QAccelerometer::AccelerationMode mode = kind == MEGLER_SENSOR_GRAVITY
                ? QAccelerometer::Gravity : QAccelerometer::User;
            accel->setAccelerationMode(mode);
            if (accel->accelerationMode() != mode) {
                qWarning() << "sensors: acceleration mode rejected for kind" << kind;
                delete sensor;
                return;
            }
        }
        if (kind == MEGLER_SENSOR_MAGNETOMETER) {
            if (!sensor->isFeatureSupported(QSensor::GeoValues)) {
                qInfo() << "sensors: calibrated magnetic field unsupported";
                delete sensor;
                return;
            }
            static_cast<QMagnetometer *>(sensor)->setReturnGeoValues(true);
        }
        devices[kind].sensor = sensor;
        megler_sensor_info entry = {};
        entry.kind = kind;
        QString description = sensor->description();
        QByteArray name = description.toUtf8();
        while (name.size() >= static_cast<int>(sizeof(entry.name))) {
            description.chop(1);
            name = description.toUtf8();
        }
        std::strncpy(entry.name, name.constData(), sizeof(entry.name) - 1);
        std::strncpy(entry.vendor, "Aurora", sizeof(entry.vendor) - 1);
        if (kind != MEGLER_SENSOR_LIGHT && kind != MEGLER_SENSOR_PROXIMITY)
            entry.min_interval_ms = minimumInterval(sensor->availableDataRates());
        info.push_back(entry);
        connect(sensor, &QSensor::sensorError, this, [kind](int error) {
            qWarning() << "sensors: backend error for kind" << kind << ":" << error;
        });
        connect(sensor, &QSensor::activeChanged, this, [this, sensor, kind] {
            if (devices[kind].running && !sensor->isActive()) {
                devices[kind].running = false;
                qWarning() << "sensors: backend stopped kind" << kind;
            }
        });
        connect(sensor, &QSensor::readingChanged, this, [this, sensor, kind] {
            if (!devices[kind].running) return;
            megler_sensor_sample result = {};
            switch (kind) {
            case MEGLER_SENSOR_ACCELEROMETER:
            case MEGLER_SENSOR_GRAVITY:
            case MEGLER_SENSOR_LINEAR_ACCELERATION: {
                const QAccelerometerReading *r = static_cast<QAccelerometer *>(sensor)->reading();
                if (!r) return;
                result = sample(kind, r->x(), r->y(), r->z());
                break;
            }
            case MEGLER_SENSOR_GYROMETER: {
                const QGyroscopeReading *r = static_cast<QGyroscope *>(sensor)->reading();
                if (!r) return;
                result = sample(kind, r->x(), r->y(), r->z());
                break;
            }
            case MEGLER_SENSOR_MAGNETOMETER: {
                const QMagnetometerReading *r = static_cast<QMagnetometer *>(sensor)->reading();
                if (!r) return;
                result = sample(kind, r->x(), r->y(), r->z());
                break;
            }
            case MEGLER_SENSOR_LIGHT: {
                const QLightReading *r = static_cast<QLightSensor *>(sensor)->reading();
                if (!r) return;
                result = sample(kind, r->lux());
                break;
            }
            case MEGLER_SENSOR_PROXIMITY: {
                const QProximityReading *r = static_cast<QProximitySensor *>(sensor)->reading();
                if (!r) return;
                result = sample(kind, r->close() ? 1 : 0);
                break;
            }
            case MEGLER_SENSOR_BAROMETER: {
                const QPressureReading *r = static_cast<QPressureSensor *>(sensor)->reading();
                if (!r) return;
                result = sample(kind, r->pressure());
                break;
            }
            default: return;
            }
            megler_sensor_push(&result);
        });
    }
};

class Host {
public:
    Host()
    {
        worker.moveToThread(&thread);
        thread.start();
        if (!QMetaObject::invokeMethod(&worker, "discover", Qt::BlockingQueuedConnection))
            qFatal("sensors: cannot dispatch discovery");
    }

    int list(megler_sensor_info *out, int max)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) return 0;
        if (!out || max <= 0) {
            qWarning() << "sensors: invalid list buffer";
            return 0;
        }
        const int count = std::min(max, static_cast<int>(worker.info.size()));
        std::copy_n(worker.info.begin(), count, out);
        return count;
    }

    void post(const char *method, int kind, int interval = 0)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) {
            qWarning() << "sensors: host is shut down";
            return;
        }
        bool ok = std::strcmp(method, "start") == 0
            ? QMetaObject::invokeMethod(&worker, method, Qt::QueuedConnection, Q_ARG(int, kind), Q_ARG(int, interval))
            : QMetaObject::invokeMethod(&worker, method, Qt::QueuedConnection, Q_ARG(int, kind));
        if (!ok) qFatal("sensors: cannot dispatch sensor command");
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) return;
        closed = true;
        if (!QMetaObject::invokeMethod(&worker, "cleanup", Qt::BlockingQueuedConnection))
            qFatal("sensors: cannot dispatch shutdown");
        thread.quit();
        thread.wait();
    }

private:
    std::mutex mutex;
    bool closed = false;
    QThread thread;
    Worker worker;
};

Host &host()
{
    // The core may retain callback pointers after shutdown.
    static Host *instance = new Host;
    return *instance;
}

int MEGLER_CB listThunk(void *user, megler_sensor_info *info, int max)
{
    return static_cast<Host *>(user)->list(info, max);
}
void MEGLER_CB startThunk(void *user, int kind, int interval)
{
    static_cast<Host *>(user)->post("start", kind, interval);
}
void MEGLER_CB stopThunk(void *user, int kind)
{
    static_cast<Host *>(user)->post("stop", kind);
}
}

megler_sensor_host auroraSensorHost()
{
    if (!QCoreApplication::instance()) qFatal("sensors: QCoreApplication is required");
    megler_sensor_host result = {};
    result.user = &host();
    result.list = listThunk;
    result.start = startThunk;
    result.stop = stopThunk;
    return result;
}

void auroraSensorShutdown()
{
    host().shutdown();
}

#include "sensor_host.moc"
