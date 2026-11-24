# AndroidCompatibilityLayer for Aurora

The app is displayed as Android App Manager (Менеджер приложений Android in Russian).
The installed application ID is `ru.androidcompatibilitylayer.appmanager`.
It differs from earlier builds, so existing private data is not automatically
migrated and previously created shortcut RPMs may no longer work.

The inherited Megler core API and bundled runtime keep their original names.
The installation-status marker `MEGLER_INSTALL_STATUS_V1` also remains unchanged
because it is consumed by the bundled framework DEX.

The manager and standalone app interface use Russian when the device language
is Russian and otherwise use English. Android apps choose their own language;
the Android language setting is independent of the manager interface.

At Android runtime startup, the bundled certificate authorities are combined
with Aurora's extracted server-auth TLS bundle. The p11-kit blacklist and the
bundled store's known distrust list are excluded. System certificates with an
`nss-server-distrust-after` restriction are imported only if they were already
in the bundled Android store: the Android store cannot express that restriction.
Certificates installed through Aurora's system trust configuration are included
after the system regenerates its TLS bundle; a separate per-user NSS database
is not treated as an Android user CA store. A private read-only snapshot is
published atomically, and new processes see changes on their next start.
If the bundle is absent, the built-in certificates remain available; errors
reading an existing bundle or system policy are reported instead of silently
ignoring them. The diagnostic overlay reports CA startup status.

Android notifications from a running app are sent to the system notification
service when its notification permission and channel allow them. Channel
importance `MIN` and `LOW` maps to low urgency without sound or a preview;
`DEFAULT` has normal urgency and no preview, while `HIGH` and `MAX` permit a
normal-urgency preview. A channel with no sound suppresses sound independently
of importance. Legacy apps use notification priority and sound settings. Aurora
decides whether to display previews, and has no exact equivalent of Android's
status-bar visibility or channel controls.

Updates and removals are tracked while that app process remains alive. Progress
and ongoing notifications request no automatic expiry; progress updates and
notifications marked `onlyAlertOnce` suppress repeat previews and sound. Ongoing
notifications remain manually dismissible to avoid leaving an unremovable
notification if the app exits. Custom sounds, vibration, notification actions,
and PendingIntent taps are not supported. Foreground services publish their
notifications through the same bridge. The Android package is supplied as
`x-nemo-origin-package`; compatible Lipstick implementations suppress default
vibration for Android notifications, independently of `suppress-sound`.
The app and hosted microG service managers clean up foreground notifications
when they destroy a service, even if its `onDestroy` does not call the superclass.
Notifications left after a process exit cannot yet be updated or removed by a
later process. These mappings and their presentation require device verification.

When microG is selected, the manager starts and retains its shared worker even
without an attached client, and retries after a worker failure while the manager
is running. A RuntimeManager periodic task is registered with a 15-minute
interval and autostart. If the manager's D-Bus service is absent, it attempts to
start a detached background manager; otherwise it asks the manager to check and
restart the shared worker. When microG is deselected, the manager requests that
the task be disabled. Failed scheduling or disable requests are logged and
retried every 30 seconds while the manager remains running; operations are
serialized and converge on the latest selection.
The watchdog task omits Camera and Audio permissions, which the SDK disallows
for background tasks; the main application's permissions are unchanged.
Aurora may suspend or stop the background manager, including after
the task exits; uninterrupted operation, restart after reboot and permissions
require device verification. This does not yet deliver FCM to stopped Android
apps; registration and real push delivery on Aurora remain unverified.

Settings contains a manual manager update check. It includes published GitHub
prereleases, but not drafts, and downloads only a newer RPM matching the
manager's build ABI, independently of the kernel architecture. The download
shows progress and is checked against GitHub's size and SHA-256 metadata before
opening the system installer for confirmation.
An optional startup check, disabled by default and limited to once per six
hours, notifies about a newer version without downloading or installing it.
The verified RPM remains in Downloads. The checksum verifies integrity against
GitHub metadata, not an independent signature; system package checks still apply.
While a standalone RPM is being prepared, the matching app page shows the
current phase and an indeterminate progress indicator. Away from that page,
including when the manager is minimized, a system notification shows the
package and phase without inventing a percentage. A separate transient banner
announces each reported phase. The progress notification is removed when the
page becomes visible again or the build ends; errors are reported separately.

The log menu can pause detailed capture without clearing existing entries.
The report header identifies the RPM architecture from the running executable's
build ABI, rather than the device CPU; unknown builds are reported as such.
Standalone processes receive the paused state through the existing polling
channel, so they may take a moment to stop recording. Capture resumes when
the manager restarts; this does not control system logs outside the manager.
Diagnostic frame-pair capture is available only while detailed logging is
active, not while it is paused. The control follows the same interprocess
state polling, so changes may take a moment to reach an open preview.
Disabling detailed logging clears the current manager history and shared
snapshots but retains exported files in Documents. Clearing the log folder also
clears the current manager history; active apps may recreate shared snapshots.
The exported report is limited to 10,000,000 bytes. Standalone snapshot and
pending event limits allow up to that much formatted event data per process;
on overflow, the report indicates lost events. Events that do not fit in an
exported report remain available for a subsequent export.
Consecutive identical standalone errors are summarized with repeat counts and
the last occurrence time; errors from different sources remain separate.
The Android log channel splits longer messages into packets and reconstructs
them on a dedicated receiver thread. Its queue is bounded to 10,000,000 bytes
including per-message accounting. A saturated socket can still lose messages
before the queue fills; such losses and queue overflow are reported. The
legacy D-Bus transport also has independent queue and rate limits. Core log
formatting can truncate messages before they reach the channel.

For Google Maps (`com.google.android.apps.maps`), the runtime attempts to
instrument both its Java Cronet fallback URL requests and Android's system
OkHttp `HttpURLConnection`. With detailed logging enabled, `maps-net:`
records identify a request or connection within one process and engine. The
Java Cronet path records HTTP status, redirects, outcome, coarse error category
and bytes read; a zero status means unavailable, bytes read may be less than
the complete response, and redirects are not included in the count.
The system HTTP path records the connection and the first final HTTP status or
coarse error; it does not measure response size or request completion.
System HTTP events can include other Maps traffic, and a single request can
appear in both paths: their identifiers are independent.

For the first 16 system HTTP failures with an IOException directly caused by
an otherwise unclassified checked exception, `checked_group` groups exception
classes by process-local numbers (up to eight classes), without logging names;
additional classes are `overflow`.
`checked_scope` is a coarse package category, and `provider_frame` reports only
whether one of a few fixed ProviderInstaller class names occurs in the first
48 frames of that exception. `not_seen` does not exclude obfuscated or
truncated frames. `gms_provider` reports whether `GmsCore_OpenSSL` is registered
at that moment, not whether it is active or whether provider installation
caused the failure. `checked_sample=limit_reached` marks unsampled failures.
`cause_origin` is the first recognized subsystem frame in the deepest sampled
cause, not necessarily where the exception originated.

These records do not contain URLs, headers, bodies, credentials or coordinates.
`ready` indicates the expected hooks were installed; `unavailable` includes the
fixed name of the failed stage, and `instrumentation_error` indicates
incomplete capture. Each collector tracks at most 2,048 active objects and
emits at most 10,000 events per process; `capacity_exceeded` and
`event_limit_reached` indicate incomplete capture. A system HTTP connection is
tracked until disconnect or garbage collection; a client keeping many
connections open can fill the system HTTP collector.
Native Cronet, gRPC bidirectional streams, other engines and requests in other
processes are not covered. Hook installation and actual coverage depend on the
app and runtime versions and have not been verified on Aurora.

To collect them, enable logging in the manager, start Maps, reproduce the
problem, then choose Log > Save to file. The report in Documents includes
available standalone snapshots. Inspect the entire report before sharing:
other existing log entries can still contain sensitive information. Pausing
or disabling capture affects the manager's report as described above, not
system output; exported files remain in Documents until deleted separately.

Native EGL/GLES rendering is always built into the Aurora runtime, including
Aurora IDE builds. New runtime features do not require experimental build or
environment switches; diagnostic fixtures do not replace normal application
execution. Device support is still subject to validation.
Android GL frames use system EGL pbuffers and CPU readback for preview; this
is not the Windows GPU compositor or zero-copy presentation. The default EGL
display, device drivers and actual Android apps still need device validation.
Native-activity frame readback currently requires the same draw and read surface;
multisampled window configurations are refused, and composition with
native-activity Java overlays is not implemented.
If EGL initialization fails, GL contexts are unavailable; non-GL CPU drawing
continues to use the existing preview path.
The optional `android_compatibility_layer_native_egl_system_test` target exercises pbuffer rendering
and native-window frame delivery against the system EGL; it requires an EGL
display and GLES driver. On a headless Mesa system, run it with
`EGL_PLATFORM=surfaceless`. This software-renderer test does not verify a
device GPU.

The runtime includes an EGLImage-to-Qt scenegraph
path for native-activity window frames and Java SurfaceView layers. It copies
EGL frames on the GPU; the premultiplied Java views image is uploaded from CPU
memory. It falls back
to CPU readback if the Qt context, display, extensions, import or handoff are
unavailable. The whole-window path sets alpha to opaque without changing RGB,
matching the CPU preview; SurfaceView layers preserve premultiplied alpha when
placed above the Java views as translucent. The handoff waits for Qt, normally
with a one-second timeout; driver calls and lock contention may take longer.
After a fallback, other SurfaceView layers cannot recover their last GPU-only
frames until they swap again. Qt scenegraph interoperation, concurrent layers,
and device drivers have not been verified on an Aurora device. This is an
experimental device test configuration, not a complete GPU implementation, and
remains experimental.
Back-gesture frame comparison cannot detect changed GPU-only frames, so its
existing timeout may be used instead.

The Android GPU Canvas path is also always included.
If the shared graphics context or presentation is unavailable,
the Canvas path falls back to CPU rendering. When combined with SurfaceView,
the Canvas texture occupies the same composition position as the CPU views:
above ordinary SurfaceView layers and below layers placed on top. This path
has been exercised on an Aurora device with standalone RuStore, but other
devices and applications can behave differently. The former experimental CMake
options no longer control the build, including values retained in an existing
CMake cache.

microG settings open as a page in the manager, where the back gesture can start
anywhere on the page. The Android runtime remains in a separate process.
Aurora Store and the embedded microG settings page attempt GPU Canvas
presentation automatically. CPU rendering
and the bounded CPU frame channel remain available as fallback.
Embedded previews exchange CPU fallback pixels through a private file in
`/dev/shm` when available. The authenticated
socket still carries the frame notice and ACK; the reader copies the pixels
before acknowledging them. If shared storage cannot be used, frames travel
over the socket as before. This CPU transport does not replace GPU rendering.

GPU diagnostics are explicit, non-installed test targets, not manager launch
modes. The former manager GPU probe switch has been removed. EGL DMA-BUF,
GBM/import and Qt scene-graph checks remain in the GPU transport, import and
state tests. Capability detection in the runtime does not initialize Hybris
gralloc or use it for frames: the platform ABI and peer import are unverified.
Advertised extensions alone do not prove GPU screen output.

Embedded workers probe the current EGL context for DMA-BUF export support.
If direct export is unavailable, the one-time `Embedded GPU GBM probe` checks
allocation and rendering at the current preview size on a visible DRM render
node. If successful, each live frame is copied on the GPU into a freshly
allocated ARGB8888 GBM buffer, imported as a GLES render target, then sent to
Qt as DMA-BUF. Allocation and copy are currently per-frame, not a buffer pool.
The frame transport itself does not read pixels back on the successful GPU
path; unsupported Canvas operations can still use their CPU fallbacks.
The Android core may also read back before Canvas self-copy, even when the
copy itself runs on the GPU. GPU presentation therefore does not mean every
Canvas operation is CPU-free.
The one-time probe checks local GBM capability; it does not send a discarded
test frame instead of live application output. The former probe-only
environment override is no longer read by the manager or worker.
For live frames, the parent verifies the actual format and modifier against its EGL
importer, imports the file descriptors, and copies into a parent-owned GPU
texture before acknowledging buffer release. A credential-checked local
`SOCK_SEQPACKET` channel carries the descriptors and per-plane format, stride,
offset, and modifier metadata; EGL handles and process pointers are never
serialized. Only one frame may be in flight, producer and consumer GPU work is
completed before buffer reuse, and runtime handoff/import failure falls back
to CPU texture readback and the existing CPU frame channel. The GPU path is
logged as active only after a live GPU frame is imported and acknowledged. This
experimental path still requires device validation; it does not establish
DMA-BUF support on Aurora ARM64.

For live device verification, build and deploy normally in Aurora IDE.
Launch the manager, open Aurora Store and then microG settings, and
inspect each worker's log. `Embedded GPU DMA-BUF frame imported and
acknowledged` confirms a live frame reached Qt; a fallback warning identifies
failure instead. Exercise redraw, resize/rotation, input and back navigation
in both views, and compare the actual screen with CPU presentation. An ACK
alone does not verify visible pixels or the absence of per-operation CPU
fallbacks. Device GPU/GBM and peer import are not yet verified by this change.
The optional `android_compatibility_layer_embedded_gpu_import_test --gbm-live`
exercises live GBM copying, pixel import, resizing and rejection without a Qt
scene graph; exit code 77 means no suitable GBM/EGL device is available.
Returning from the microG page stops that process after the transition;
cancelling the gesture leaves it running.

Preview orientation and size changes are coalesced in one event-loop turn.
The runtime receives the final viewport and display rotation together, including
embedded workers, instead of relaying intermediate width/height or rotation-only
configurations during a page rotation. This avoids redundant Android relayouts;
the app still needs to render a frame for the new configuration. Standalone
Android orientation requests now constrain the standalone window too. Each
generated standalone RPM contains the runtime executable present at build time;
updating the manager alone does not establish that an already installed package
uses the updated executable. Rebuild and reinstall that RPM to test the current
runtime.

Aurora Store keeps its own window while its Android process restarts. The
supervisor recognizes the ProcessPhoenix default-launcher restart used by
known versions of the store; other restart intents are not replayed. It limits
restarts to three per minute, and does not restart after a crash or user close.
If an accepted restart's worker does not exit within 500 ms, the
supervisor terminates it and waits for its exit before starting the replacement.
On manual close, after the worker confirms a successful Android exit and host
shutdown, it has 500 ms to finish teardown. Without that confirmation the
existing seven-second stop watchdog still applies. Manual close never restarts it.
This path still requires validation with the installed store on an Aurora device.
Before Qt tears down the offscreen Canvas surface, the Android main and frame
threads release their own GL contexts. This applies to both pixel and remote
texture presentation, including fallback from remote texture to pixels.
If the worker remains alive after
`host shutdown complete`, its `teardown:`
messages show which destructor boundaries it reached. If it logged `leaving main`
but was still killed, look at process-exit cleanup rather than the Android stop
or host shutdown. GPU builds log which threads release a Canvas context and
how many drawing contexts remain immediately before offscreen surface destruction.
A live owner entry gives the thread ID, OS thread name, first Canvas operation,
and whether its context was ever bound to the surface or is being released.
A zero count rules out a still-live Canvas drawing context at that point, not
the Qt share context or the platform surface cleanup itself. The supervisor still
reports the actual process exit status.

For the current device experiment, the RPM spec explicitly enables all three
options, including when its build directory has cached values from an earlier
build. The detailed log records the GL vendor, renderer and version when the
shared Canvas context is created. Rendering artifacts may occur.
IDE options for a separate CMake build do not change the packaged
configuration.

`tools/native-egl-image-test.c` tests the transport for copying
a pbuffer into an EGL image and importing it in an independent GLES context,
using an EGL fence without CPU pixel transfer. It reads pixels only to verify
the result. Build the optional `android_compatibility_layer_native_egl_image_test` target in the
SDK and run it with a working EGL display.
The test requires EGL image and fence extensions and `GL_OES_EGL_image`; an
unsupported or unavailable display is reported as a failure. Passing does
not verify Qt scenegraph interoperation, device hardware, or production frame
composition.

Open the repository's `CMakeLists.txt` in Aurora IDE and select an aarch64
Aurora SDK kit matching the device's OS version. Build and deploy the
`ru.androidcompatibilitylayer.appmanager` application and launch it on the device.
If the debug launcher passes the desktop `%u` field code as its only argument,
the manager treats it as an ordinary launch, not an APK request.

The RPM build disables optional test targets to speed up IDE configuration.
For tests, configure CMake separately with `ANDROID_COMPATIBILITY_LAYER_BUILD_TEST_TARGETS=ON`
(the default for direct CMake builds), then build the desired test target.
Debug builds use reduced debug information for the bundled SQLite amalgamation
to limit build time and object size; other sources keep their normal debug info.

Logging is off by default. In the Log tab, select **Enable logs** and confirm
the existing consent dialog to record detailed diagnostics. The warning calls
out that raw logs may contain passwords, tokens, app data, paths and network
addresses, and that standalone snapshots are stored in the shared organization
folder, which other apps in that organization can access. Disabling logging
stops new entries but does not delete saved files.

Standalone processes use the manager's trusted D-Bus `State` only to control
capture. New RPMs publish bounded, atomic per-instance snapshots under
`Aurora::Application::organizationFilesDir(false)`; they do not send raw
standalone events through D-Bus `Append` and do not request `Documents` or
`UserDirs` permissions for logging. New RPM metadata enables snapshots
independently of the logging state at build time. Older RPMs without the
`standaloneSnapshots` metadata flag retain D-Bus `Append`; rebuild and install
them once if you want to switch to shared snapshots. Rebuilding is not part of
routine log collection. Existing `fileDiagnostics` metadata remains supported for
those older packages. The shared folder is untrusted: snapshots
are validated as bounded ordinary files, but their contents and claimed
application identity are not cryptographically authenticated. The manager
refreshes these snapshots in the Log view. Built-in preview/Store logging
retains its existing D-Bus transport.

The Log view shows the combined report in export order. Search filters its
lines case-insensitively without changing the saved report. The Android window
shows up to six short, redacted diagnostic messages while capture is active;
it currently sees only events recorded in the window's own process. It does
not expose raw log lines, Intent contents, or tokens in the overlay.
The feature-tagged rules and their fixed display text are in `overlayRules`
in `aurora/error_log.cpp`: add an entry when a feature needs testing and
remove its entries after the feature is confirmed. Keep the display text
free of private data, and update the focused overlay tests for rule changes.

The detailed report identifies an unconfigured or unavailable snapshot directory
and an empty directory; an empty directory can also mean that snapshots were
already exported. Bounded snapshots retain an initial startup window and prefer
keeping errors when dropping older entries, with a loss count in the report.
Events produced before the standalone process receives the manager's confirmed
logging state are not retained, even if logging is subsequently enabled.

**Save to file** atomically writes one combined
`AndroidCompatibilityLayer-*.txt` report under
`Documents/AndroidCompatibilityLayer-Error-Logs`, with manager history and
available standalone snapshots. It does not import arbitrary files or older
exports from Documents. A partial export is marked in the report and leaves
omitted snapshots in place. After a successful save, only included snapshots
that did not change during export are removed from the organization folder; if removal
fails, the export still succeeds and the UI warns separately. A failed save
leaves the source snapshots untouched. Legacy `StandaloneDiagnostic-*.txt`
files in Documents are not imported automatically.

**Clear log folder** deletes matching saved manager/legacy standalone files in
Documents and standalone snapshots in the organization folder after
confirmation; it does not clear the current journal or unrelated files. Close
diagnostic apps first because active writers may recreate snapshots.
**Send to developer** opens the public Yandex form inside the manager using
Aurora WebView (Chromium). The
form and its system file pickers support portrait and landscape. Single-file
requests use the regular picker; multi-file requests use the system
`MultiFilePickerDialog` and return the full selected list to the form. The form
may still limit how many files it accepts. Check the saved combined report for
private data, then select and submit it manually.
The embedded page returns to the log view when the form reaches its success
page with an answer key. Attachments and failed submissions do not close it.
Opening the form does not create or attach a report, or submit the form. For apps started by the
manager, process IDs and completion, failure or forced-stop categories help
correlate lifecycle events; standalone processes do not have a manager
observing their termination. Raw output may still appear in the IDE. Older
reports created before this change are not modified and may contain private
data. The manager declares the WebView's `Internet`, `UserDirs` and
`DeviceInfo` permissions and depends on the system `ru.auroraos.webview`
package. The manager links against the system WebView using `aurorawebview`
pkg-config metadata, retaining its system CEF search path. The dynamic loader
loads CEF when the manager process starts so it can allocate static thread-local
storage. The manager initializes the system WebView with the original command
line before creating Qt's application; CEF subprocesses enter this same early
initialization rather than launching another manager UI. The report button only
opens the page and does not initialize CEF again. Preview, import and builder actions again launch the manager binary;
they load CEF at startup but do not initialize WebView. The CEF-free
`/usr/share/ru.androidcompatibilitylayer.appmanager/ru.androidcompatibilitylayer.appmanager.runtime`
is a non-executable payload copied into new standalone RPMs.

Bundle inspection detects Java bytecode in the base and accepted split APKs,
including feature splits whose base has no DEX. Native ABI checks still apply.
This fixes rejection of that bundle layout, but does not establish that a
particular Chrome release installs or runs: its original complete bundle and
device execution are still required for that conclusion.
Focused regressions: build and run
`android_compatibility_layer_report_webview_startup_test` with the SDK, and run
`python3 core/tests/apk-split-code/test.py` on a Linux x86_64 host with GCC and
zlib development files. Add `--manager /path/to/built/manager` to exercise
the real store bundle importer with split-only code and rejection cases.

The manager holds bounded activity and detailed histories in RAM and periodically
atomically saves them in its private application files directory. They are
restored when the manager restarts. The oldest output is discarded when a
buffer fills and this is marked in the report. Snapshot write and recovery
errors are shown in the Log page; a malformed snapshot is not overwritten.
The snapshot and its single reusable pending file are each limited to 96 MiB.
Preview/Store and older standalone processes send events to the manager over an
append-only D-Bus interface; it does not offer remote log reads. New standalone
RPMs do not send log content over D-Bus. The manager remains running
when its window closes, but Aurora may suspend or terminate it. Events received
since the most recent snapshot (normally up to five seconds, or longer if
writes fail or the process is suspended) can be lost. Delivery is best effort:
queue limits, unavailable services, process termination and platform background
policies can cause loss. Known drops are reported where possible. The interface
uses system-provided process identity for routing, not proof of authorship: remote
events are untrusted and can be forged. Per-source quotas limit their impact.
For log submission the manager requires a nonempty key ID of at most 256
characters and accepts only
its own executable (also used by the built-in Store worker) or a generated
standalone executable, in either the legacy `/usr/bin` layout or the versioned
`/opt/app/<app-id>/<version>/bin/<app-id>` package layout.
Older standalone packages that predate the current shared-snapshot format need
to be rebuilt once to use organization snapshots; this is a compatibility
migration, not a step in the log-submission instructions. Device verification
of activation and sandbox permissions is still required.
Reports include per-manager-process counts of state and append requests,
accepted calls and rejected or limited requests without sender identities.
Clients also send bounded, predefined diagnostic stages and failure categories
without log contents to the manager's public service name, before manager
identification and when their connection status changes. These aggregate counts
are untrusted and may be forged; they do not prove which app sent them. No
log events are sent before manager identification. If the diagnostic request
itself cannot reach the manager, zero requests cannot distinguish an app that
did not start from a connection failure. Counters reset when the manager
process restarts. Standalone processes also emit sanitized connection-failure
stages to stderr (for example in the IDE).
If manager identification fails, the client reports whether the executable
matched and whether the key was empty or oversized, without printing either
value. It also compares the unique and well-known names once for diagnosis;
the comparison does not authorize log delivery.
Detailed events observed after the client starts but before it learns the
manager's detailed setting are not retained; if the setting is enabled, their
count is reported after a connection succeeds. Earlier startup events are not
counted.

The Log page pulley can enable detailed logging after a privacy warning. While
enabled, unfiltered Qt, Android runtime and captured stdout/stderr may include
credentials, application data, paths and network addresses. Its state and
history are also saved privately. Disabling it clears the detailed history and
attempts to save the cleared state immediately; if this fails, the previous
snapshot can reappear after restart. Saved or shared copies are not cleared.
State propagation is
polled, so output around mode changes may be missed or buffered; native output
may be duplicated. No new `activity.log` or `detailed.log` files are written;
the private snapshot has a separate name.
Existing files and older packages are not modified; older packages may continue
using their previous on-disk logging until rebuilt.

GPU Canvas performance summaries are emitted on the next completed frame
after each roughly three-second interval while extended logging is active in
the rendering process. Timing samples are still taken on one in sixteen frames.
For embedded GPU Canvas, `producer_us` includes readback but not submission;
`readback_present` counts presentation readbacks, `readback_gl_us` and
`readback_convert_us` split sampled readback time. On ARM64 the CPU conversion
uses NEON to swap RGBA to BGRA while flipping the rows. `pixel_submit_*` measure
the subsequent CPU pixel submission callback. `qt_wait_us` applies to the
shared-texture path only. These are elapsed times, not GPU execution times.
`readback_cache_*` counts operations satisfied by an up-to-date CPU mirror,
separately from readback attempts. For filters, compare `readback_cache_filter`
with `readback_filter` to see how often a readback was skipped.
`filter_fallback_mode`, `filter_fallback_blend`, `filter_fallback_self`,
`filter_fallback_gles` and `filter_fallback_invalid` count why a color-filter
draw used the CPU path; `filter_draw_errors` records GLES errors after the
filtered draw was submitted. Shared-texture presentation does not read pixels
back; `readback_present` applies to the CPU pixel-presentation callback.
The GPU Canvas applies all declared Android color-filter blend
modes (`CLEAR` through `LIGHTEN`) with paint `SRC_OVER` directly to a GPU
source texture for shape and external-bitmap draws. For the outer paint blend,
`DST` is a no-op; `SRC_OVER`, `DST_OVER`, `DST_OUT`, `SRC_ATOP`, `XOR`, `PLUS`
and `SCREEN` use premultiplied GLES blend factors when the selected filter
preserves source coverage (`CLEAR`, `DST`, `SRC_IN`, `DST_IN`, `DST_OUT`,
`SRC_ATOP` or `MODULATE`). Other filter/blend combinations, invalid filter
modes and GLES setup failures retain the CPU fallback. `canvas_copies_gpu`
counts Canvas copies that sampled a GPU snapshot instead of reading the source
bitmap back. Integer-aligned 1:1 copies support paint opacity and supported color
filters and blend modes. Standalone GLES 2 and 3 also support nearest-neighbor
scaled copies at integer-aligned source and
destination coordinates when no destination pixel center lands on a source
pixel boundary, including independently scaled axes and destinations cropped
at target bounds. With `FILTER_BITMAP`, scaled copies use bilinear filtering
on a GPU snapshot cropped to the source rectangle, preventing samples outside
it. Copies can use a source written by another shared context. Both copy paths
support integer rectangular clips, including closed rectangular paths and up to
eight rectangular difference clips. Ambiguous nearest-neighbor sampling,
nonrectangular or fractional path clips, unsupported paint combinations,
unsupported context combinations, reversed destination coordinates, and shader
setup failures (including unavailable fragment high precision) keep the existing
CPU fallback.
For Bitmap-aware GPU hosts, a Bitmap's texture persists across Canvas
operations. Flush orders GPU work without necessarily updating CPU pixels;
CPU pixel access, encoding and operations that need a CPU image materialize
the current pixels. CPU writes invalidate the GPU copy before its next use.
GLES 3 shared-context writes use sync fences for cross-context ordering;
contexts without sync-object support retain the blocking completion fallback.
Core self-draws do not force a readback: eligible draws, including overlapping
copies and standalone GLES 2/3 scaling, use a GPU snapshot; unsupported cases materialize
the CPU image as needed. Text shaping, measurement and glyph-outline generation
remain CPU-side through Qt, while the GPU Canvas renders the resulting outlines.
Embedded previews use CPU Canvas from startup if the GPU channel or EGL
DMA-BUF export is unavailable. They do not use GPU readback as the startup
fallback: readback still needs a current EGL context on the drawing thread.
Standalone shared-texture presentation is unaffected.

Older standalone RPMs may continue their previous `StandaloneDiagnostic-*.txt`
behavior in Documents, or may continue sending raw events through the D-Bus
append interface. New RPMs use the shared-snapshot format and do not require a
logging option at build time. Their snapshots are bounded and atomically
replaced per process. They can contain private data and are accessible to
other apps in the same organization; the manager treats both the folder and
snapshot contents as untrusted. Existing legacy files are left untouched and
are not folded into exports automatically.

Standalone RPM generation is available on an app's library details page.
Settings can also automatically create an RPM and open the system installer
after a successful library import. This option is off by default. Imports
are queued for RPM generation if another build is in progress; the system
installer may require confirmation, and opening it does not confirm installation.
A successful import displays a banner when the library is not currently visible.
An indeterminate progress bar shows the current inspection, build, signing and
save stages; it does not estimate remaining time or report installer completion.
The SDK signer appends a CMS trailer; the SDK's generic `rpm -Kv` reports
digest mismatches for signed packages, including SDK-built packages. Signed
test packages install and their headers and files verify in an isolated SDK RPM
database with signature checks disabled. Signature acceptance and standalone
launch still require device verification. Each generated RPM
has a new application identity, its own executable, APK (including installed
splits), runtime, icon and private data. It starts with empty data and does
not copy or delete the original app's data. Close the Android app before
generating its RPM. Symbolic links to files and directories are packaged as
independent copies; inaccessible or cyclic links and links to the original
app's private data are rejected.
The RPM is signed with the public Developer Mode test key,
saved with the app in the manager's library, and opened in the system installer
for confirmation. The saved RPMs are removed with the library entry; avoid
removing it while the installer is still reading a package. Whether the system
installer can access the private storage requires device verification. Each package includes a full runtime and requires substantial
free space; its signature and standalone launch require device verification.
Older shortcut RPMs may share app data with the main app.

On a repeat request, the manager reuses its last signed RPM only if its contents
and generation inputs still match, including the manager RPM version and release,
APK and splits, launcher, runtime ABI, QML, settings and app permissions. Cache
entries created before manager-version tracking are rebuilt once. Otherwise it builds a new package and
removes the previous internal RPM after publishing the replacement. Importing
an identical single APK retains the cache; an approved different APK for the
same package replaces the library copy and invalidates that cache. Existing
split installations cannot be replaced by a single APK. Installed standalone
packages and their data are not removed by these operations.

The app's Permissions page selects allow or deny for dangerous permission groups
declared by the APK. The selection is embedded into newly generated RPMs; it
does not change RPMs already installed. Android permission checks use the
selection, and denying camera, location or nearby devices also disables the
corresponding device bridge and Aurora permission. Denying any shared media or
storage group disables all shared-folder bridges, not private app storage.
File-picker access selected by the user remains available. These controls are
not a security sandbox: not every Android or native-code access path is covered.
The host's own Aurora permissions also apply. Device behavior requires verification.

At Android app startup, the runtime uses the current Aurora ambience highlight
colour as the seed for Material You system colours. Apps that use their own
colours may not follow it; ambience changes take effect on the next launch.

When a standalone Android app requests installation of a single APK, Megler
copies a bounded snapshot to Downloads and opens it with the system file handler.
Select Android App Manager if necessary. The manager inspects and saves
its own APK copy in the library, where an RPM can be created again or the saved
copy removed. Removing a library entry does not uninstall a standalone RPM.
The exported file remains in Downloads. The handoff cannot report the result of
the system installation to the requesting Android app, so that app receives a
failure status even when the file handler opens. Split APK installation requests
are not supported by this handoff. This flow requires device verification.

The manager also registers for XAPK and APKS files. On systems that identify
these bundles as ZIP, it is offered as a ZIP handler too; ordinary `.zip` files
are rejected. Opening a bundle inspects it before adding its APKs to the
library. File association and handling require device
verification.

Android apps run in their own preview windows. Text uses Qt's system fonts
and glyph outlines. Common filled text uses antialiasing; other text styles
still use the test renderer. Embedded TrueType and OpenType fonts are
registered with Qt for the preview process lifetime. Qt selects fonts by
family and style, so a name collision can select a different face; font
collections and variations are not fully supported. Colour emoji are not
supported. Other graphics, including gradients, are
simplified. This is not a production graphics backend. The preview uses a
full-screen surface at the window's native pixel size, without an on-screen
Back or Close menu. The Aurora back swipe sends Android Back
experimentally; device verification is still needed for touch gestures,
keyboard dismissal and picker cancellation. After a completed swipe, a back
arrow remains visible until a frame different from the one captured at Back
is received, or for up to five seconds. This is only a best-effort
approximation: the different frame might be unrelated to Back or already
in flight. A picker request interrupts the wait. If the image does not change,
the preview returns to the current frame at timeout without closing the app.
The primary mouse button is translated to touch, including dragging; the mouse
Back button invokes Android Back. Wheel
input is approximated by short touch swipes; it works only where an app
responds to touch scrolling and may not behave like a native mouse wheel.
Hover, secondary and middle buttons, and Android mouse-specific events are
not supported. Test physical mouse input on the device.
Image decoding and encoding use Qt's image codecs, including PNG and JPEG
when the corresponding plugins are installed.
The experimental keyboard bridge forwards committed text and editing keys;
complex input methods and text replacement remain limited.
Audio output uses Qt Multimedia for stereo PCM; device routing, automatic
recovery and background media policy are not implemented.
The preview's MediaCodec host currently provides MP3 only, not H.264, H.265
or AAC. Its whole-file media host decodes MP3 audio through Qt Multimedia's
QAudioDecoder when the device has a compatible decoder plugin; it does not
decode video or AAC. Seeking restarts MP3 decoding from the beginning and
discards samples up to the requested position, so long seeks can be slow
and fail after a timeout. Only little-endian 16-bit mono or stereo decoder
output is accepted; unsupported formats and decoder failures are logged.
Duration is provided by the Qt backend and may be approximate or unavailable.
The host has been exercised in the x86 SDK but not on an Aurora device.
Android video playback is not yet supported. The shared MediaCodec input normalizer accepts
Annex-B and 1-, 2- or 4-byte MP4 NAL lengths from avcC/hvcC records for a
future video decoder; this does not add a decoder or verify device playback.
ARM64 and ARMv7 builds include Android System WebView in the shared runtime. It is
separate from Aurora's native WebView, which handles the manager's feedback form.
The experimental camera bridge supports CPU-accessible NV12/NV21 preview
frames. Autofocus, macro focus and torch are available only when the Qt camera
backend reports the required capabilities. Android Camera and camera2 JPEG
captures are encoded from preview frames, not full-resolution sensor stills.
When Wayland does not report a native screen orientation, the Android display
uses a portrait reference, consistent with the camera bridge's existing fallback.
This is an assumption and may be incorrect on a landscape-native device.
An experimental sensor bridge exposes supported physical sensors. Android file
requests support single and multiple selection through the Aurora picker.
Image capture requests open a QtMultimedia camera screen with explicit capture,
confirmation and cancellation; its still-image quality depends on the device
backend. Video capture requests are not supported. The camera screen requires
the QtMultimedia 5.6 QML plugin at runtime. Camera controls, the capture
screen and multiple selection require verification on a device.
The experimental Bluetooth LE bridge reads adapter state and discovery results
from BlueZ over system D-Bus. Aurora's `Bluetooth` permission is required.
BlueZ's exposed device properties are converted into approximate advertising
data; the original packet bytes, manufacturer/service data, and connectability
are not included. Active and passive scans use the same
BlueZ discovery operation. GATT connections, service discovery, reads, writes,
notifications, MTU requests, pairing, and opening Bluetooth settings are not
implemented and report failure. Test adapter access and scan behavior on a
device; the local test covers only address and advertising-data conversion.
If an Android app does not stop within five seconds, its isolated process exits
with an error; the controller reports the failure.
The optional `android_compatibility_layer_preview_text_test` CMake target exercises glyph outlines
and text measurements; run it in a Qt GUI environment with real system fonts.
Pass one or more TrueType or OpenType file paths as arguments to test embedded
font loading, source-byte lifetime, derivation and concurrent registration.

**Add APK or bundle** imports a standalone APK or an `.apks`/`.xapk` bundle
into private app storage after inspection. Bundle splits are stored with the
base APK and supplied to the Android runtime when the app launches; compatible
expansion files stay in that app's private storage. The importer keeps splits
for the runtime's supported ABI, nearby screen densities, selected languages
and feature splits up to the runtime limit. APKMirror `.apkm` bundles are not
supported. The Library lists apps alphabetically and shows the most used apps
first; the limit is configurable from 0 to 10 (default 5).
Tablets use two columns and offer even limits, rounding up any odd limit saved
on a phone.
Tap an app to open or switch to it.
Its context menu offers app information, force stop and removal with data.
The back gesture starts only at the screen edge, allowing horizontal swipes
elsewhere in the app. Aurora Store uses a 16 dp edge zone; other apps use 32 dp.
The first three completed back gestures show a short hint on the previous page.
Apps persist across restarts and run in separate processes with their own data
directories. Aurora may decline window activation; use the system app switcher
in that case. Close an app before removing it. Bundle and standalone APK
updates are not supported; an existing installation and its data are left
unchanged if the same package is selected again. This does not install
applications into Aurora.
Closing a standalone app window pauses its Android activities. Megler keeps
that process alive only while it has an active service, scheduled job or alarm,
and the launcher icon's `Start` intent can reopen the window while the process
still exists. This state is process-local: Aurora may suspend or terminate the
process, and jobs or alarms are not restored after termination or reboot. This
does not provide FCM delivery or guarantee continuous background execution.
Only trusted APKs and bundles should be used: Android app permissions and
isolation are not implemented, so code can access Megler's Aurora permissions
and data.
Megler requests Aurora's Internet, UserDirs, Camera, Sensors and Audio permissions; Android apps can
use these permissions too.
Standalone RPMs instead derive Aurora permissions from the APK's requested Android permissions
and the selected permission groups. Internet is included when requested by the APK; Audio is
included for playback. Android groups without an Aurora equivalent cannot be mapped one-to-one,
and selecting a permission does not implement an unsupported Android feature. Permissions are
fixed when the RPM is built. Without access to shared folders, APK handoff and saving error logs
to Documents may be unavailable.
The Device tab shows the operating system, CPU architecture and Qt screen size;
settings can limit the longer side of the Android rendering surface, select
the phone's language and region, English (United States), or another BCP 47
tag, select the phone's, light or dark theme, and select the phone's location
or enter fixed coordinates. For standalone RPMs, these choices are captured
when the RPM is created and cannot be changed afterward; system language,
theme and location are resolved by the standalone app at each start.
Changes otherwise take effect the next time an app is opened. The default
surface size is native.
Device location uses Qt Positioning and the Aurora Location permission.
Its source, device permissions and updates require verification on Aurora.
The host permission is shared with the Android guest; it does not provide
individual Android-app consent isolation. Fixed coordinates simulate a position.
The experimental microG option is disabled by default.
Selecting the provider does not open or download microG. When opened with
Configure, it downloads the latest official GitHub release if no microG APK
is selected. Updates can download the release or select a local
APK. Downloads are checked against GitHub release metadata; local updates work
offline after explicit confirmation. APK signatures are not verified. Updates preserve app data and
require all running Android apps to be closed. The downloaded microG APK is
used by subsequently opened apps. Google account sign-in through microG has
been confirmed on an Aurora ARM64 tablet; that does not confirm individual
Google APIs or Play Integrity. No Aurora Wi-Fi scan host is
connected for Android apps. In microG settings, a completed Aurora back
gesture returns to the manager and closes the settings instead of sending
Android Back. A canceled gesture leaves the settings running.

The manager also exposes an experimental D-Bus `GetProvider` method at
`/ru/androidcompatibilitylayer/appmanager/microg` on
`ru.androidcompatibilitylayer.appmanager.microg`. It returns only the manager's
selected `none` or `microg` setting; it does not check whether microG is installed
or working. The D-Bus launch command starts the manager without creating its
window. A normal launch or an incoming APK request creates the window in that
same process. An idle windowless process schedules exit after 60 seconds;
system suspension or termination may change that timing. The desktop launcher,
D-Bus activation, sandbox permissions, simultaneous clients and APK handling
still need device verification. Closing the window exits the manager; a later
request may activate it again.

New standalone exports made while `microg` is selected opt into the shared
worker using `sharedMicrog: true` in their metadata (except the microG app itself).
Old exports without that field and exports made with `none` retain their previous
behaviour. Changing the provider invalidates the export cache; re-export an old
standalone app to obtain the new D-Bus permissions and startup contract.

`OpenClientChannel()` takes no arguments and returns one Unix stream FD plus the
manager-resolved UID, virtual PID and package, only after the worker accepts that
same identity. The standalone verifies the reply's unique D-Bus sender through
Aurora `IdentifyByBusName` and holds its dedicated connection until guest teardown.
The host transfers the bootstrap directly into the core, not through guest
arguments or environment. The guest installs its UID before ApplicationInfo is
cached and its Binder router before Application construction or provider startup.
Shared GMS binds fail explicitly on unsupported intents or disconnect; they do not
silently start a local microG. Outside this opt-in, the local path is unchanged.

The optional CMake target `android_compatibility_layer_shared_microg_startup_client`
opens the same verified channel and runs a real guest APK through
`megler_run_headless`, without BinderGuestTest-specific expectations. Its arguments
are `APK DATA ANDROID_ROOT PICTURES WAIT_MS`; `PICTURES` is reserved for fixture
compatibility and unused. It logs the manager's UID/PID/package and guest output,
stops after `WAIT_MS`, and waits up to another 15 seconds for guest teardown before
failing. `--bootstrap-check` checks native FD validation and ownership separately.

The export registry is a package mapping, not APK attestation. PackageManager's
synthetic GMS version/signature are not verified metadata of the worker APK.
This is not proof that Google login, arbitrary Google APIs or Play Integrity work.
The real Aurora/Sailjail identity, D-Bus FD forwarding and sandbox permissions
still require device verification. The separate `dbus-identity` test clients
provide an identity check without accounts or secrets.

The experimental `InspectCaller` method resolves the actual D-Bus sender using
Aurora's `IdentifyByBusName` and returns that sender, executable and key ID only
to the caller. It accepts no claimed identity and limits concurrent lookups.
Errors are returned instead of treating unidentified callers as trusted.
Identification is not authorization: a key ID, especially the public developer
test key, does not establish trust in an APK. Nothing in this method grants
access to account data, and device sandbox behaviour still needs verification.

Standalone APK handoffs and child log forwarding check the manager's unique
D-Bus identity against the legacy `/usr/bin/ru.androidcompatibilitylayer.appmanager`
path or the versioned `/opt/app/ru.androidcompatibilitylayer.appmanager/<version>/bin/ru.androidcompatibilitylayer.appmanager`
path. Both require a nonempty key ID of at most 256 characters.

For local checks, build `android_compatibility_layer_manager_lifecycle_test`
with test targets enabled. Run the binary under `dbus-run-session`, and run
`sh tools/manager-lifecycle-test.sh <path-to-test-binary>` for activation and
restart on an isolated bus. The optional `--actual-manager <path-to-manager>`
test mode uses Qt's `minimal` platform plugin and a temporary configuration.
These checks do not reproduce the device sandbox or launcher.

The manager's cover shows the library and offers actions to choose an APK,
save the log, and open Aurora Store. The library pulley also opens the store.
Aurora Store is hidden from the manager's library view but remains in the
underlying library. It runs in a separate preview process and window; reopening
it activates the existing window. Opening an installed app from this Store window
queues the launch without waiting on the Store UI thread; the manager still
handles launch failures, including a missing standalone installation. The
RPM installation offer follows the current manager page in portrait and
landscape and keeps both actions visible when the message needs to scroll. The
immediate callback result indicates that the request was queued, not that the
app started; a failed or unverified manager reply is logged.
The desktop entry allows separate multitasking
covers for the manager and store. Settings allow installing or updating it
from GitLab or a local APK. These imports stay in the manager's library even
when automatic standalone RPM installation is enabled. Signatures are not
verified. Updates preserve app data and require the store to be closed. The Android store's operation,
sign-in and compatibility on Aurora have not been verified.
Online microG and Aurora Store downloads show their progress in Settings and in
system notifications. Release checks, unknown download sizes and APK
inspection use an indeterminate indicator; closing the app stops its downloads.

The Log tab shows recent controller and app output in a bounded in-memory
buffer, with explicit notices when output is truncated or cannot be forwarded.
It can be copied or shared on request. Logs may contain private information;
review them before sharing. Capture and sharing require device verification.
The experimental renderer, runtime and lifecycle support remain incomplete;
passing inspection does not guarantee that an app will run correctly.

The library can launch different APKs in separate Megler processes, with their
own data directories and installation channels. A per-data-directory lock
prevents concurrent use of the same package by cooperating Megler processes.
Close individual apps independently;
Megler's library window remains open until all apps have closed so that
installation requests can still be confirmed. This does not create separate
system menu icons or Android security isolation. Window management and
controller close behavior still require verification on an Aurora device.

SQLite is compiled into Megler using its upstream host-layer VFS. Before
building, run `tools/sqlite-fetch.sh` and copy `build/sqlite/sqlite3.c` and
`build/sqlite/sqlite3.h` into `aurora/sqlite`. Include this directory when
Aurora IDE creates a source archive. The verified source is generated data
and is not committed to Git. SQLite is public domain.

The prepared Android runtime is required when building the project. By default,
ARM64 uses `aurora/android-runtime` and ARMv7 uses
`build/android9/root-armeabi-v7a`; include the appropriate directory when
Aurora IDE creates its RPM source archive. These are generated data and are not
committed to Git. Set the CMake `ANDROID_ROOT` cache entry only to override the
automatic selection; clear an older cached value to restore it. To regenerate
the ARM64 runtime,
prepare `build/android9/root-arm64-v8a` with the upstream Android 9 tools,
including the ARM64 native libraries and Java framework, then copy that root
into `aurora/android-runtime`.
The upstream scripts require Android SDK build
tools, a JDK 8 and the Android NDK; see `tools/android9-*.sh`,
`tools/build-framework.sh` and `tools/dvm-android-root.sh`. Android runtime
files come from the Android 9 image and AOSP sources; their applicable licenses
and notices must be considered before redistributing a package.

## Android System WebView

The selected backend is Android's `WebViewFactory`, not Aurora's native WebView
and not the old WKWebView bridge. The abandoned Android-to-QML/Aurora bridge and
its mock tests have been removed; the independent native diagnostic remains
documented in `aurora/webview_native_probe.md`.

CMake installs the selected Android runtime once, with its ABI marker, into
`/usr/share/common/ru.androidcompatibilitylayer/appmanager/android-runtime`.
The manager and new standalone packages resolve it with
`Aurora::Application::organizationPathTo`. They must be signed with the same
key for access; signing or native library loading in an ordinary Aurora sandbox
still requires verification on a device. Previously generated standalone RPMs
continue to use their own bundled runtime. New RPMs do not include runtime files,
but retain their APKs, splits, settings and private storage layout. They record
the current marker from `aurora/standalone-runtime-abi`; increment it whenever a
runtime change is incompatible with existing standalone binaries. A missing or
inaccessible runtime is reported separately from an ABI mismatch. Recreating an
RPM gives it a new application identity and does not migrate private data.
Megler mounts the selected root at the guest path `/system`. Rebuilding
`build/android9/root-arm64-v8a` alone does not update the IDE source runtime.

For Aurora aarch64 the primary ABI is `arm64-v8a` (the bundled
`lib/libjavacore.so` is ELF64/AArch64). The required provider is a standalone
APK at `app/webview/webview.apk` inside the selected root, visible to Android as
`/system/app/webview/webview.apk`. It must contain `classes.dex`, its compiled
manifest/resources, and `lib/arm64-v8a/<library>` named by manifest metadata
`com.android.webview.WebViewLibrary`. The usual library is
`libwebviewchromium.so`. The current framework requires target SDK >= 33 and
the entry class
`com.android.webview.chromium.WebViewChromiumFactoryProviderForT`.
A split/Trichrome package with external code or library dependencies is not a
drop-in provider for this loader. The upstream standalone package name is
`com.android.webview`; this factory reads the name from the manifest rather
than enforcing that name. It does not implement Android's WebViewUpdateService
or authenticate the APK's signing certificate.

The factory invokes the provider's public static
`create(WebViewDelegate)` method. The APK manifest parser reads package name,
`targetSdkVersion` and application-level metadata
`com.android.webview.WebViewLibrary`; the latter names the shared library
(normally `libwebviewchromium.so`). Provider DEX must contain
`com.android.webview.chromium.WebViewChromiumFactoryProviderForT`. The loader
does not enforce a particular package name or minimum version code. The exact
native lookup is
`/system/app/webview/webview.apk!/lib/arm64-v8a/libwebviewchromium.so` on
ARM64; it must be an ELF64 little-endian AArch64 shared object. On ARMv7 the
path instead uses `armeabi-v7a` and the library must be ELF32 ARM. Other required native
dependencies must also exist in the APK or runtime. The
`libwebviewchromium_plat_support.so` system-library name is currently a stub
backed by Megler native hooks, not an APK substitute. This runtime uses an
Android 9 (API 28) root, so a provider must also be compatible with that
runtime's Java APIs; the factory does not validate `minSdkVersion`.
The installed Android SDK/platform are API 36, but they supply no WebView APK;
no emulator system image is installed either.

Before configuring an ARM Aurora build, fetch the
[pinned AOSP Chromium 128.0.6613.88 provider](https://android.googlesource.com/platform/external/chromium-webview/+/27ac2bdb9a1a4e55dc65bb6cdc752dbfdea3b9f4/)
with `tools/webview-fetch.sh --aosp-chromium-128 arm64-v8a` for ARM64 or
`tools/webview-fetch.sh --aosp-chromium-128 armeabi-v7a` for ARMv7. The local
build input is `build/webview/arm64-v8a/webview.apk` or `build/webview/webview.apk`,
respectively; it is not checked into Git or
downloaded by CMake. Aurora IDE's standard apptool build uses the source tree
in place; if building from a source archive instead, include this ignored APK
at the same relative path as well as the selected Android runtime and
`aurora/sqlite`. The ordinary Aurora RPM spec works without a local wrapper
or a WebView flag. CMake verifies the pinned SHA-256 and stages the APK and
its platform support stub in the manager's installed shared runtime.
Non-ARM builds do not stage this provider. ARMv7 requires a separate complete
`armeabi-v7a` Android runtime; its WebView and standalone RPMs have not yet
been verified on a device.

Applications using that shared runtime can attempt to use the standard Android
WebView API without an allowlist or per-app provider copy. Standalone packages
already using the compatible shared runtime pick it up when the manager is
updated; older packages with an embedded runtime must be rebuilt to use the
shared provider. App data remains per application; rebuilding a standalone
package under a new identity does not migrate it. Existing network permissions
still apply.

As in upstream Megler, the renderer runs in the Android application's
process, not in a separate renderer sandbox. The prebuilt Chromium 128 is
not on an automatic security update channel. Support for an app's WebView
does not imply compatibility with every site or sign-in flow. Before public
binary redistribution, review the
[Chromium license](https://chromium.googlesource.com/chromium/src/+/128.0.6613.88/LICENSE)
and the applicable third-party notices and terms for this prebuilt.

The factory now reports a missing provider with its guest path and primary ABI.
Before creating its class loader or invoking native constructors it checks the
existing manifest target-SDK requirement, DEX magic and the named library's
little-endian ELF class, machine and shared-object type. Invalid metadata,
missing ABI libraries, split/odex-only packages and malformed headers fail with
explicit diagnostics. This is a structural preflight, not signature validation,
full ELF/DEX validation, a compatibility guarantee or a sandbox test. Changed
framework sources must be rebuilt with `tools/build-framework.sh` and copied to
the prepared runtime by the runtime maintainer; these changes do not publish
DEX or modify the existing generated runtime.

The focused host test is `core/tests/webview-provider/WebViewProviderCheckTest.java`.
Compile it together with
`core/framework-overlay/android/webkit/WebViewProviderCheck.java` using a JDK
(`javac -source 8 -target 8 -d <temporary-output> <both-source-files>`), then run
`java -cp <temporary-output> android.webkit.WebViewProviderCheckTest`.
It uses synthetic ZIP/DEX/ELF headers only and does not execute Chromium.

Local pages, loopback, controlled HTTPS, JavaScript and text input were
verified on the ARM64 tablet with the same provider.
Build the test APK at `build/guest/java/webview-smoke.apk` with
`tools/build-test-app.sh --force webview-smoke` and import it from the Android
app library. On its details page choose **Install as RPM**, confirm in the
system installer, then launch the installed app from the Aurora desktop/app
menu; APKs cannot be launched directly from the library. The standalone RPM
references the shared runtime and does not carry its own provider. Its pages
are local APK assets and require no network permission, account or JavaScript
bridge. The test-app script accepts
`MEGLER_ANDROID_SDK`, `MEGLER_BUILD_TOOLS_VERSION` and `MEGLER_ANDROID_PLATFORM`
overrides for the installed Android SDK. Its manifest opens and stays in
landscape. It creates a standard Android `WebView`, opens
`file:///android_asset/index.html`, and shows lifecycle/load events. Use the
local page link or Page 2, Back, Forward and Reload buttons to check navigation.
Leave the app and return to check pause/resume; close it to check destroy.

For a separate loopback-only network test, build
`tools/build-test-app.sh --force webview-loopback` and import
`build/guest/java/webview-loopback.apk`. Install it from its library details
page and launch it from the desktop as above. It binds an HTTP server to
`127.0.0.1` on a random port and serves two static pages; its test-only
network policy permits cleartext only to that address. JavaScript and external
navigation are disabled. The on-screen events and `WebViewLoopbackSmoke` log
show server startup and requests, page loads, navigation and errors. Check Page
2, Back, Forward and Reload. `LOAD_FINISH` alone is not proof of success:
also check the displayed page, server response and any `LOAD_ERROR`. This
does not test DNS, HTTPS, external content or renderer isolation.

For the separate HTTPS test, build
`tools/build-test-app.sh --force webview-https`, then import and install
`build/guest/java/webview-https.apk` from the library and launch it from the
desktop. The default address is `https://ya.ru/robots.txt`, a plain-text
endpoint that avoids the home page's redirects. To test another trusted address
without rebuilding, enter it in the address field and tap Load. The test only
allows a main-frame GET to the entered HTTPS address. Navigations, redirects
and all subresources are blocked, and JavaScript is disabled. Normal
TLS verification remains in effect. `WebViewHttpsSmoke` reports load, HTTP
error, certificate and visual-readiness events without logging requested URLs.
The displayed `PROBE_HTTP_STATUS` is from a separate HTTPS request with
redirects disabled: Android WebView does not expose a successful main-frame
HTTP status through these callbacks. `VISUAL_READY` means ready for the next
draw, not that pixels were actually displayed; check the page on screen.
This test has not established renderer isolation or production suitability.

The same APK also has an **Offline JS** button. It opens a separate WebView
with a bundled page, blocked network, no file or content access and no JavaScript
bridge. Wait for **JavaScript ready**, then tap **Tap to test**; the visible
counter should increment. Type a short sample in the text field, move the caret
into the middle, insert a character and delete one. The on-page preview should
match after each edit, including with the on-screen keyboard in landscape.
The text is not logged or sent; the `WebViewOfflineJsSmoke` tag reports only
the JavaScript startup and tap events. JavaScript stays disabled in the HTTPS test.

`WebViewDelegate.isMultiProcessEnabled()` currently returns `false`, so
Chromium runs its renderer on threads in the embedding app process. This
diagnostic does not establish an OS renderer sandbox and must not be treated as
suitable for real accounts or untrusted browsing. Do not add `--no-sandbox`,
bypass TLS/signature/trust policy or use real credentials to make the smoke
test pass. A separate renderer-process boundary and a secure provider update
and qualification plan remain outstanding.

The compatibility layer cannot run arbitrary APKs reliably. Development tests
remain separate from the application build.
The experimental port does not yet provide a complete Aurora graphics backend
or Android application isolation and lifecycle integration.

The repository is licensed under GPL-3.0; see the root `LICENSE`.
