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

Native EGL/GLES rendering is experimental and disabled by default. In the
Aurora IDE, set the CMake option `ANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_NATIVE_EGL=ON` to try it.
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

For experiments only, setting both `ANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_NATIVE_EGL=ON` and
`ANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_GPU_COMPOSITION=ON` enables an EGLImage-to-Qt scenegraph
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
must not be enabled by default.
Back-gesture frame comparison cannot detect changed GPU-only frames, so its
existing timeout may be used instead.

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

The RPM build disables optional test targets to speed up IDE configuration.
For tests, configure CMake separately with `ANDROID_COMPATIBILITY_LAYER_BUILD_TEST_TARGETS=ON`
(the default for direct CMake builds), then build the desired test target.
Debug builds use reduced debug information for the bundled SQLite amalgamation
to limit build time and object size; other sources keep their normal debug info.

The manager and standalone Android apps save observed Qt warnings and errors,
Android runtime error messages and reported startup failures as text files in the
shared `Documents/AndroidCompatibilityLayer-Error-Logs` directory. Each filename contains the app
name and a timestamp. Reports include recent diagnostic context, which may
contain private data or credentials; review them before sharing. They remain
until deleted by the user. An inaccessible Documents directory or a failed
write is reported in the diagnostic output. Abrupt termination, unreported
errors, and messages lost when the runtime's nonblocking log channel is full
may not produce a report. Access on an Aurora device still needs verification.
The Log page's pulley menu can also save its current contents to this folder as
a separate text file; the result or a write error is shown on the page.

Standalone RPM generation is available on an app's library details page.
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
Android System WebView is not included in the Aurora runtime. The original
Megler WebView requires its Android Chromium package and runtime integration;
Aurora's native WebView is not currently connected to Android WebView calls.
The experimental camera bridge supports CPU-accessible NV12/NV21 preview
frames; focus, torch and other camera formats are not yet supported.
An experimental sensor bridge exposes supported physical sensors. Android
file requests can select one file through the Aurora picker; multiple files
and camera capture requests are not supported.
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
used by subsequently opened apps. The runtime attempts to open its settings,
but microG, Google account authentication, Play Integrity, and individual
Google APIs have not been verified on Aurora. No Aurora Wi-Fi scan host is
connected for Android apps.

Aurora Store appears in the library after installation. Settings allow
installing or updating it from GitLab or a local APK. Local
updates require confirmation; signatures are not verified. Updates preserve
app data and require the store to be closed. The Android store's operation,
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

The prepared Android runtime is required in `aurora/android-runtime` when
building the project, including when Aurora IDE creates its RPM source
archive. It is generated data and is not committed to Git. To regenerate it,
prepare `build/android9/root-arm64-v8a` with the upstream Android 9 tools,
including the ARM64 native libraries and Java framework, then copy that root
into `aurora/android-runtime`.
The upstream scripts require Android SDK build
tools, a JDK 8 and the Android NDK; see `tools/android9-*.sh`,
`tools/build-framework.sh` and `tools/dvm-android-root.sh`. Android runtime
files come from the Android 9 image and AOSP sources; their applicable licenses
and notices must be considered before redistributing a package.

The compatibility layer cannot run arbitrary APKs reliably. Development tests
remain separate from the application build.
The experimental port does not yet provide a complete Aurora graphics backend
or Android application isolation and lifecycle integration.

The repository is licensed under GPL-3.0; see the root `LICENSE`.
