# Experimental Aurora sensor bridge

Integration, deliberately not applied to shared files:

1. Add `aurora/sensor_host.cpp` and `aurora/sensor_host.h` to the application
   target and ensure CMake AUTOMOC is enabled. Add `Sensors` to
   `find_package(Qt5 COMPONENTS ... REQUIRED)` and link `Qt5::Sensors`.
   The Aurora public API documentation specifies
   `BuildRequires: pkgconfig(Qt5Sensors)` and the `Sensors` desktop
   permission; add these to the existing spec and `[X-Application]`
   `Permissions` respectively, preserving other entries.
2. Include `sensor_host.h` in the preview and set
   `run.sensors = auroraSensorHost()` before `megler_run`. Create the host
   only after `QCoreApplication` exists. Discovery runs once on its Qt worker
   thread; the first host creation waits for discovery.
3. Call `auroraSensorShutdown()` after the preview has stopped and before
   destroying the application. Do not call it from a sensor callback or
   the sensor worker. Shutdown drains queued commands, stops and deletes
   sensors on their owning thread, and joins it. It is permanent and
   idempotent; old callbacks are safe but cannot restart the host.

Supported when a Qt backend connects: combined accelerometer, gyroscope,
calibrated magnetometer, lux light sensor, boolean proximity and pressure.
Gravity and linear acceleration are exposed only if the backend advertises
acceleration-mode support and accepts the corresponding mode. Calibrated
magnetometer is exposed only with `GeoValues`; raw readings are not
misrepresented as calibrated. Absolute/relative rotation and inclinometer
are intentionally absent: Qt's Euler rotation readings do not establish the
quaternion/reference-frame or inclinometer convention required by Megler.
Unavailable backends are omitted, and unsupported requests, backend errors,
busy and failed starts are logged. The host interface has no callback to
report a later start failure to Android and does not retry automatically.
Hardware and backend support can differ even on devices running Aurora.

Qt fixed-orientation readings are converted to the Windows-unit contract
expected by Megler: acceleration m/s² to opposite-sign g, magnetometer
tesla to microtesla, pressure pascal to hectopascal; gyroscope degrees/s
and light lux pass through. Proximity has no distance, so it reports -1 and
the core uses its fallback range. Qt timestamps have an unspecified epoch:
the bridge uses zero so the core timestamps delivery rather than inventing
an acquisition time. Metadata ranges, resolution and hardware vendor are
unknown; `Aurora` identifies the host and the core supplies fallback ranges.
Rates are selected from Qt's advertised ranges where available. Rate
changes restart the backend, and actual scheduling remains backend-defined.
Qt must produce the initial reading; the bridge does not fabricate one from
an uninitialized cache, so change-only backends must be checked on device.

`Sensors` grants the **host process** access through sensorfw. Android guests
using this bridge receive physical sensor readings under that host permission;
the bridge does not provide per-guest consent or additional permission
isolation. Verify the target's permission policy and disclose this access to
users before enabling it. An absent permission, missing plugin, unavailable
hardware, or a busy sensor can prevent readings. Test with both an Aurora
device and the [sensor emulator](https://developer.auroraos.ru/doc/sdk/app_development/emulation/sensors),
including permission denial, pause/resume, rate changes, near/far, and sensor
axes against a native app. The fake-backend test is not a hardware test.

Build `aurora/tests/sensor_host_test.cpp` as a standalone executable with
Qt5 Core, Sensors and threads. It includes the implementation directly, so
do not also compile `sensor_host.cpp` into that executable. Generate
`sensor_host.moc` from `sensor_host.cpp` with Qt's moc and put it on the
include path. It checks discovery, unit conversions, requested rates,
cross-thread commands, shutdown and late callbacks.

References: [Aurora dependency and permission table](https://developer.auroraos.ru/doc/software_development/reference/public_api/dependency_connection),
[Qt 5.6 QSensor](https://doc.qt.io/archives/qt-5.6/qsensor.html),
[Qt 5.6 sensor timestamps](https://doc.qt.io/archives/qt-5.6/qsensorreading.html).
