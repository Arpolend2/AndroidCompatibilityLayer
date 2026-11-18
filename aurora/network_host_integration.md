# Aurora network host

The Android framework reads the latest process-local snapshot through
`megler.NetworkNative`. The Qt host publishes it from the Qt thread; the
native snapshot is atomic. For compatibility, shells that do not publish retain
the previous synthetic Wi-Fi behavior. The Aurora preview constructs the host
before starting Android, so Aurora receives an actual snapshot or unknown;
after the host shuts down the state is unknown.

Integration:

1. The application target includes `aurora/network_host.cpp` and links
   `Qt5::Network` and `Qt5::DBus`; the core includes
   `core/src/drivers/mg_java_wifi.c`.
2. The Android child constructs exactly one `AuroraNetworkHost`
   on the Qt application thread after `QGuiApplication` is created and before
   `megler_run`. Keep it alive while the Android app runs. Call `shutdown()`
   on that same thread before destroying the Qt application; it is idempotent.
3. Keep the framework DEX and core in sync. After changing network Java
   sources or `framework-overlay/megler/NetworkNative.java`, rebuild and deploy
   every generated DEX part with the Android root. Do not mix the framework
   with an older core lacking the native registration.
4. On the device, verify unknown startup, Wi-Fi, mobile data, airplane mode,
   network changes, callback unregister and shutdown. A build or SDK package
   does not establish that the device's network backend reports these states.

Qt's active configuration indicates an active bearer, not necessarily
internet access. Aurora's `isOnline()` probes a configured address and is used
as evidence for `NET_CAPABILITY_INTERNET`. ConnMan services are queried
asynchronously over the system D-Bus every two seconds; only a single connected
service is considered unambiguous. For that service matching the single active
Qt bearer, its `ready` or `online` state plus
Qt's successful probe maps to `NET_CAPABILITY_VALIDATED`. ConnMan stays `ready`
when its own online check is not configured, and WorkManager (used by Aurora
Store downloads) cancels work without VALIDATED. This is not a guarantee of
internet access: the probe endpoint is configurable, including a private one.
For that same matching default route, a favorite Wi-Fi connection, a cellular
connection or Ethernet maps to `NET_CAPABILITY_TRUSTED`. ConnMan's favorite
status means a successful connection was saved; it is an approximation of
Android's implicit user trust, not an assessment of network security.
No extra external connectivity probe is performed. Multiple
active configurations, unsupported bearers, or no known configuration do not
invent a transport. No known configuration and a failed probe yield unknown;
known inactive configurations and a failed probe yield offline. Check these
semantics against the device backend, particularly when saved configurations
remain visible offline.

Android reports no active network for unknown or offline. Existing callbacks
wait through unknown; a known offline state emits `onLost`. Callbacks are
polled every 500 ms and dispatched through their requested handler. This
represents one default network rather than Android's complete network set.
Metering, VPN, roaming, bandwidth, signal strength, network binding and
connectivity broadcasts are not determined by this bridge. Trust and validation
remain unknown if ConnMan is unavailable or inaccessible, multiple services are
connected, the route cannot be matched,
or its status does not establish them. Unknown
capabilities are not asserted; consequently requests requiring such capabilities
(including default `NetworkRequest.Builder` capabilities) or forbidding an
unknown capability may not match. This conservative limitation requires
device/API work rather than synthetic answers.

For requests requiring both internet and validation, the `MeglerNetwork`
diagnostic records missing capabilities and transport mismatches once per
distinct combination. The host logs snapshot changes without network names.

Focused tests are `aurora/tests/network_snapshot_test.cpp`,
`network_native_test.c`, `network_host_test.cpp`, `network_connman_dbus_test.cpp` and
`network_java/NetworkConnectivityTest.java`. The Java test uses test-only
network and handler substitutes; run against separately compiled framework
classes, not the shared DEX. Run the D-Bus test in a session bus, for example
with `dbus-run-session`.
