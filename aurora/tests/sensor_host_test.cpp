// Standalone Qt Sensors backend test; see sensor_host_integration.md.
#include "../sensor_host.cpp"

#include <QSensorBackend>
#include <QSensorManager>

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <thread>

namespace {
std::mutex eventsMutex;
std::condition_variable changed;
std::vector<megler_sensor_sample> events;
int starts = 0;
int stops = 0;
std::atomic<bool> failNext(false);

class FakeBackend : public QSensorBackend {
public:
    explicit FakeBackend(QSensor *sensor) : QSensorBackend(sensor)
    {
        addDataRate(10, 100);
        setDescription(QStringLiteral("test sensor"));
        if (sensor->type() == QAccelerometer::type)
            accel = setReading<QAccelerometerReading>(nullptr);
        else if (sensor->type() == QGyroscope::type)
            gyro = setReading<QGyroscopeReading>(nullptr);
        else if (sensor->type() == QMagnetometer::type)
            magnet = setReading<QMagnetometerReading>(nullptr);
        else if (sensor->type() == QLightSensor::type)
            light = setReading<QLightReading>(nullptr);
        else if (sensor->type() == QProximitySensor::type)
            proximity = setReading<QProximityReading>(nullptr);
        else if (sensor->type() == QPressureSensor::type)
            pressure = setReading<QPressureReading>(nullptr);
    }

    bool isFeatureSupported(QSensor::Feature feature) const override
    {
        return feature == QSensor::AccelerationMode || feature == QSensor::GeoValues;
    }

    void start() override
    {
        if (failNext.exchange(false)) {
            sensorBusy();
            std::lock_guard<std::mutex> lock(eventsMutex);
            ++starts;
            changed.notify_all();
            return;
        }
        if (accel) { accel->setX(9.80665); accel->setY(-19.6133); accel->setZ(0); }
        if (gyro) { gyro->setX(90); gyro->setY(-45); }
        if (magnet) { magnet->setX(0.00005); magnet->setY(-0.00003); }
        if (light) light->setLux(123);
        if (proximity) proximity->setClose(true);
        if (pressure) pressure->setPressure(101325);
        newReadingAvailable();
        {
            std::lock_guard<std::mutex> lock(eventsMutex);
            ++starts;
        }
        changed.notify_all();
    }

    void stop() override
    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        ++stops;
        changed.notify_all();
    }

private:
    QAccelerometerReading *accel = nullptr;
    QGyroscopeReading *gyro = nullptr;
    QMagnetometerReading *magnet = nullptr;
    QLightReading *light = nullptr;
    QProximityReading *proximity = nullptr;
    QPressureReading *pressure = nullptr;
};

class FakeFactory : public QSensorBackendFactory {
public:
    QSensorBackend *createBackend(QSensor *sensor) override { return new FakeBackend(sensor); }
};

bool near(double a, double b) { return std::abs(a - b) < 0.00001; }

bool waitForStarts(int count)
{
    std::unique_lock<std::mutex> lock(eventsMutex);
    return changed.wait_for(lock, std::chrono::seconds(5), [count] { return starts >= count; });
}
}

extern "C" void megler_sensor_push(const megler_sensor_sample *value)
{
    std::lock_guard<std::mutex> lock(eventsMutex);
    events.push_back(*value);
    changed.notify_all();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    FakeFactory factory;
    const QByteArray id("megler-test");
    const QByteArray types[] = {QAccelerometer::type, QGyroscope::type, QMagnetometer::type,
                                QLightSensor::type, QProximitySensor::type, QPressureSensor::type};
    for (const QByteArray &type : types) {
        QSensorManager::registerBackend(type, id, &factory);
        QSensorManager::setDefaultBackend(type, id);
    }

    assert(rateForInterval(0, qrangelist()) == 0);
    qrangelist rates;
    rates << qrange(5, 10) << qrange(20, 50);
    assert(rateForInterval(0, rates) == 50);
    assert(rateForInterval(67, rates) == 20);
    assert(rateForInterval(1, rates) == 50);
    assert(minimumInterval(rates) == 20);
    rates.clear();
    rates << qrange(10, 60);
    assert(minimumInterval(rates) == 17);

    megler_sensor_host api = auroraSensorHost();
    megler_sensor_info found[16] = {};
    assert(api.list(api.user, found, 16) == 8);
    for (int i = 0; i < 8; ++i) {
        assert(found[i].kind != MEGLER_SENSOR_ORIENTATION);
        assert(found[i].kind != MEGLER_SENSOR_RELATIVE_ORIENTATION);
        assert(found[i].kind != MEGLER_SENSOR_INCLINOMETER);
    }
    assert(api.list(api.user, found, 2) == 2);
    assert(api.list(api.user, nullptr, 2) == 0);
    const int kinds[] = {MEGLER_SENSOR_ACCELEROMETER, MEGLER_SENSOR_GYROMETER,
                         MEGLER_SENSOR_MAGNETOMETER, MEGLER_SENSOR_LIGHT,
                         MEGLER_SENSOR_PROXIMITY, MEGLER_SENSOR_BAROMETER};
    for (int kind : kinds) api.start(api.user, kind, 20);
    assert(waitForStarts(6));
    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        assert(events.size() == 6);
        assert(near(events[0].values[0], -1) && near(events[0].values[1], 2));
        assert(near(events[1].values[0], 90) && near(events[1].values[1], -45));
        assert(near(events[2].values[0], 50) && near(events[2].values[1], -30));
        assert(near(events[3].values[0], 123));
        assert(near(events[4].values[0], 1) && near(events[4].values[1], -1));
        assert(near(events[5].values[0], 1013.25));
        for (const megler_sensor_sample &event : events) assert(event.time_ns == 0);
    }
    api.start(api.user, MEGLER_SENSOR_ACCELEROMETER, 40);
    assert(waitForStarts(7));
    failNext = true;
    api.start(api.user, MEGLER_SENSOR_GYROMETER, 40);
    assert(waitForStarts(8));
    api.start(api.user, MEGLER_SENSOR_GYROMETER, 40);
    assert(waitForStarts(9));
    api.stop(api.user, MEGLER_SENSOR_ACCELEROMETER);
    std::thread a([&] { for (int i = 0; i < 50; ++i) api.start(api.user, MEGLER_SENSOR_LIGHT, 20); });
    std::thread b([&] { for (int i = 0; i < 50; ++i) api.stop(api.user, MEGLER_SENSOR_LIGHT); });
    a.join();
    b.join();
    auroraSensorShutdown();
    size_t before;
    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        assert(stops > 0);
        before = events.size();
    }
    assert(api.list(api.user, found, 16) == 0);
    api.start(api.user, MEGLER_SENSOR_ACCELEROMETER, 10);
    api.stop(api.user, MEGLER_SENSOR_ACCELEROMETER);
    auroraSensorShutdown();
    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        assert(events.size() == before);
    }
    std::puts("sensor_host_test: passed");
}
