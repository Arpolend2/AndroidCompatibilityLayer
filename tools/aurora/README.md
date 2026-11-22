# Aurora cross builds

These optional Linux-host recipes build the current port against a real Aurora
SDK sysroot. They do not replace the working Aurora IDE ARM64 kit or its RPM
recipe. `rpm/ru.androidcompatibilitylayer.appmanager.spec` is the only packaging
spec; there are deliberately no per-architecture copies.

Run `bash tools/aurora/aarch64/build.sh check` first. Use the `armv7hl` wrapper
for ARMv7. With the container SDK, the defaults are:

| Input | ARM64 | ARMv7 |
| --- | --- | --- |
| `SYS` | `/opt/cross/aarch64-meego-linux-gnu/sys-root` | `/opt/cross/armv7hl-meego-linux-gnueabi/sys-root` |
| `AURORA_CROSS_PREFIX` | `/opt/cross/bin/aarch64-meego-linux-gnu-` | `/opt/cross/bin/armv7hl-meego-linux-gnueabi-` |
| Android native ABI | `arm64-v8a` | `armeabi-v7a` |

Set `SYS`, `AURORA_CROSS_PREFIX` and `AURORA_HOST_TOOLS` for a different Platform
SDK. `AURORA_HOST_TOOLS` is the directory containing runnable **host** `moc`,
`rcc`, `uic`, `qmake`, `lrelease` and `lupdate`; the container default is
`/usr/lib64/qt5/bin`. Target Qt binaries must not be executed on the host.
`pkg-config` searches only the selected target. Target CEF, Aurora WebView,
Qt and runtime-manager libraries are checked for the selected ELF ABI;
metadata selecting host `libcef.so` is rejected rather than rewritten.

By default, the IDE and these recipes select `aurora/android-runtime` for
ARM64 and `build/android9/root-armeabi-v7a` for ARMv7. CMake's `ANDROID_ROOT`
cache entry is empty by default, so the default follows the target architecture;
an explicit non-empty path overrides it. Clear an old cached `ANDROID_ROOT`
value in IDE build settings if it was set by an earlier configuration.
Supply a **complete** ABI-matched
runtime; all native ELF files are checked. APKs with native libraries must
include the selected ABI; universal APKs are allowed.
The ARM runtime check runs again during installation, including stock IDE/RPM
builds, so replacing files after configuration cannot bypass the ABI check.
The recipes do not download old release RPMs, copy their runtime, overwrite
SQLite, modify the SDK, or weaken Sailjail.

```sh
SYS=/path/to/target \
AURORA_CROSS_PREFIX=/path/to/cross/bin/aarch64-meego-linux-gnu- \
AURORA_HOST_TOOLS=/path/to/host/qt/bin \
WORKDIR=/path/to/fresh/build \
bash tools/aurora/aarch64/build.sh stage
```

Steps are `check`, `configure`, `build`, and `stage`; the default is `check`.
`configure` and later steps repeat the input checks. `JOBS` defaults to 2.
Use a separate `WORKDIR` for each architecture/SDK. An existing stage is
rejected, not recursively deleted or merged with new files.

`stage` runs the **stock CMake install** and checks:

- CEF-linked manager in `/usr/bin/<id>`, using self-spawn for workers.
- Separate CEF-free `<id>.runtime` payload, mode `0644`, in `/usr/share/<id>`.
- Shared Android runtime in `/usr/share/common/ru.androidcompatibilitylayer/appmanager/android-runtime`,
  with the current `.standalone-abi` marker and ABI-matched native ELF files.
- Required ABI-matched Android WebView APK, installed by the pinned CMake recipe.

There is no automatic RPM creation or signing step. Build/deploy through the
Aurora IDE/SDK using the stock spec. Do not package a staged tree with the PR's
old `0.2.10` specs: they omit the shared runtime, ABI marker and CEF dependency.

## ARMv7 status

The SDK must actually contain ARM hard-float Qt, Aurora WebView and CEF.
Their presence alone does not establish a working ARMv7 application: the
checked-in Android runtime is ARM64 and **must not** be reused. With it, ARMv7
configuration fails explicitly before compilation or installation.

Provide a complete Android 9 `armeabi-v7a` runtime, including the matching
native libcore/crypto/ICU libraries and framework data. The existing
`tools/android9-natives.sh armeabi-v7a` describes how to build its native part,
but that output alone is not a complete runtime. A locally prepared complete
root at `build/android9/root-armeabi-v7a` has passed the SDK/runtime checks,
full ARMv7 cross build, and stock CMake installation:

```sh
bash tools/aurora/armv7hl/build.sh check

bash tools/webview-fetch.sh --aosp-chromium-128 armeabi-v7a

WORKDIR=/path/to/fresh/armv7-build \
bash tools/aurora/armv7hl/build.sh stage
```

The verified manager and CEF-free payload are ARM ELF32 hard-float; the guest
Android libcore is ARM ELF32 soft-float, as expected for `armeabi-v7a`. The
stage includes only the matching Android runtime and the current ABI marker.
These recipes never fill missing files with ARM64 libraries.

The ARMv7 stage includes the pinned `armeabi-v7a` provider from
`build/webview/webview.apk`, not the ARM64 provider. The in-app standalone RPM
builder also supports `armv7hl` hosts and generates 32-bit RPM dependencies.
Cross-building and staging do not verify WebView, RPM installation, or Android
app execution on an ARMv7 device. QEMU user-mode can exercise ARM executables
with a compatible ARMv7 sysroot, but GUI and Sailjail testing require a suitable
ARM32 Aurora system image and emulator environment.

Input guard tests: `python3 -m unittest discover -s tools/aurora/tests`.
