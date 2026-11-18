# Aurora location host

The host provides Qt Positioning fixes from Aurora's default Geoclue source
and lets settings select a user-entered fixed coordinate. It implements
`megler_location_host`; the Android framework and its per-app location
permission checks remain unchanged.

Integration:

1. The application target includes `aurora/location_host.cpp` and links
   `Qt5::Positioning`. The RPM build needs `pkgconfig(Qt5Positioning)`, and
   the desktop entry needs the Aurora `Location` permission.
2. After `QCoreApplication` exists and before `megler_run`, get
   `megler_location_host host = auroraLocationHost()` and assign
   `run.location = host`. Keep the host alive for the process; its callback
   state is process-wide. The settings layer can call
   `auroraLocationSetFixedLocation(latitude, longitude, accuracyMeters)` for
   valid WGS 84 coordinates, or `auroraLocationUseDeviceLocation()` to restore
   the device source. A zero fixed accuracy means unknown. Apply persisted
   settings before starting the Android app.
3. Call `auroraLocationShutdown()` after the Android app stops and before
   destroying `QCoreApplication`; do not call it from an Android location
   callback. Shutdown is idempotent, and callbacks after shutdown fail safely.
4. Build the focused `megler_location_host_test` target to test fixed-coordinate
   validation, callback behavior and shutdown. Test real location updates,
   permission denial, location disabled, source timeout, and device/emulator
   Geoclue behavior on Aurora; a host build or fixed-coordinate test does not
   establish physical-source availability.

The host creates the default Qt Positioning source on its own Qt worker thread.
It preserves the source's timestamp and available accuracy, altitude, speed
and bearing attributes; unavailable attributes stay unset. A source that
supports satellites only is reported as satellites. A source supporting both
satellite and non-satellite methods is conservatively reported as network
because Qt does not identify the method used for each fused fix. Coordinates
set by the user have source `MEGLER_LOCATION_SET_BY_HAND`.

Aurora's `Location` permission is a host-process permission, not a per-Android-
app permission prompt. Android guest apps still pass through Megler's existing
Android location permission checks, but guests using the bridge receive fixes
under the host's Aurora permission. `access(ask)` cannot display a runtime
prompt. It reports `DENIED` after Qt reports `AccessError` and `NONE` when Qt
has no default source or supported positioning method. With an available
source, it reports `ON` before the first fix: the Android location worker only
starts updates after seeing `ON`.
Qt's `ClosedError` can accompany a disabled service, but does not establish
why a backend closed; the host does not claim `MEGLER_LOCATION_OFF`. Qt may
return no default source when privileges or plugins are missing, so `NONE`
does not identify which one. The first real fix may take time or never arrive,
depending on device settings, hardware, permission and provider configuration.

The implementation does not read or write Aurora's global location settings,
and does not provide per-guest consent isolation. The settings UI must make
clear that fixed coordinates simulate a position and that real coordinates
are shared with the Android app. Verify behavior on the target device,
including the emulator's positioning plugin and a denial of the Aurora
`Location` permission.
