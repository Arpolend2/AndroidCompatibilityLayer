#!/usr/bin/env python3
"""Check cross-build inputs and the stock CMake installation, without changing them."""

import argparse
import os
from pathlib import Path
import platform
import shlex
import struct
import subprocess
import sys
import zipfile


APP_ID = "ru.androidcompatibilitylayer.appmanager"
ARCHES = {
    "aarch64": (2, 183, "arm64-v8a"),
    "armv7hl": (1, 40, "armeabi-v7a"),
}
NATIVE_LIBS = (
    "libcrypto.so", "libssl.so", "libexpat.so", "libicuuc.so",
    "libicui18n.so", "libjavacore.so", "libjavacrypto.so",
    "libopenjdk.so", "libnativehelper.so",
)
RUNTIME_DATA = (
    "framework/megler-libcore.dex", "framework/core-oj.dex", "framework/core-libart.dex",
    "framework/conscrypt.dex", "framework/okhttp.dex", "framework/bouncycastle.dex",
    "framework/apache-xml.dex", "framework/megler-framework.dex",
    "framework/megler-framework-classes2.dex", "framework/framework-res.apk",
    "usr/icu/icudt60l.dat",
)


def elf_header(path):
    with Path(path).open("rb") as stream:
        header = stream.read(64)
    if header[:4] != b"\x7fELF":
        return None
    if len(header) < 52 or header[4] not in (1, 2) or header[5] != 1:
        raise ValueError("Invalid or unsupported ELF header: {}".format(path))
    if header[4] == 2 and len(header) < 64:
        raise ValueError("Invalid or unsupported ELF header: {}".format(path))
    flags_offset = 36 if header[4] == 1 else 48
    return header[4], struct.unpack_from("<H", header, 18)[0], struct.unpack_from(
        "<I", header, flags_offset)[0]


def check_elf(path, arch, sdk=False):
    actual = elf_header(path)
    expected = ARCHES[arch]
    if actual is None or actual[:2] != expected[:2]:
        raise ValueError(
            "{}: expected {} ELF{} (Android ABI {}). Use an ABI-matched runtime/SDK; "
            "do not reuse ARM64 files for ARMv7.".format(
                path, arch, expected[0] * 32, expected[2]))
    if sdk and arch == "armv7hl" and not actual[2] & 0x400:
        raise ValueError("{}: Aurora ARMv7 SDK library must use hard-float ABI".format(path))


def check_runtime(root, arch):
    root = Path(root)
    for name in NATIVE_LIBS:
        check_elf(root / "lib" / name, arch)
    for name in RUNTIME_DATA:
        if not (root / name).is_file():
            raise ValueError("Android runtime is incomplete: {}".format(root / name))
    for path in root.rglob("*"):
        if path.is_file() and elf_header(path) is not None:
            check_elf(path, arch)
    for path in root.rglob("*.apk"):
        with zipfile.ZipFile(str(path)) as apk:
            native_abis = set()
            for name in apk.namelist():
                parts = name.split("/")
                if len(parts) >= 3 and parts[0] == "lib" and name.endswith(".so"):
                    native_abis.add(parts[1])
            if native_abis and ARCHES[arch][2] not in native_abis:
                raise ValueError("{} contains only foreign Android ABIs: {}".format(
                    path, ", ".join(sorted(native_abis))))


def sdk_file(root, path):
    candidate = Path(root) / path.lstrip("/")
    try:
        candidate.resolve().relative_to(Path(root).resolve())
    except ValueError:
        raise ValueError("SDK link escapes the target sysroot: {}".format(candidate))
    return candidate


def pkg_config(*arguments):
    return subprocess.check_output(
        ["pkg-config"] + list(arguments), universal_newlines=True).strip()


def check_sdk(root, arch, host_tools, cross_prefix):
    modules = (
        "auroraapp", "aurorawebview", "runtime-manager-qt5", "Qt5Core", "Qt5Gui",
        "Qt5Qml", "Qt5Quick", "Qt5Multimedia", "Qt5Sensors", "Qt5Network",
        "Qt5DBus", "Qt5Positioning", "egl", "glesv2",
    )
    subprocess.run(["pkg-config", "--print-errors", "--exists"] + list(modules), check=True)
    for module, filename in (
        ("aurorawebview", "cef/libcef.so"),
        ("aurorawebview", "libaurorawebview.so"),
        ("auroraapp", "libauroraapp.so"),
        ("Qt5Core", "libQt5Core.so"),
        ("runtime-manager-qt5", "libruntime-manager-qt5.so"),
    ):
        libdir = pkg_config("--variable=libdir", module)
        # Some SDK metadata already includes pc_sysrootdir in variable values.
        if libdir.startswith(str(Path(root).resolve()) + "/"):
            libdir = libdir[len(str(Path(root).resolve())):]
        check_elf(sdk_file(root, libdir + "/" + filename), arch, sdk=True)
    cef_flags = shlex.split(pkg_config("--libs", "aurorawebview"))
    cef_files = [flag for flag in cef_flags if flag.endswith("/cef/libcef.so")]
    if len(cef_files) != 1:
        raise ValueError("aurorawebview.pc must specify its target libcef.so explicitly")
    cef = Path(cef_files[0])
    try:
        cef.resolve().relative_to(Path(root).resolve())
    except ValueError:
        raise ValueError(
            "aurorawebview.pc selects host CEF: {}. Use SDK metadata with "
            "${{pc_sysrootdir}}; do not link the host library.".format(cef))
    check_elf(cef, arch, sdk=True)
    machine = subprocess.check_output(
        [cross_prefix + "gcc", "-dumpmachine"], universal_newlines=True).strip()
    if not machine.startswith("aarch64" if arch == "aarch64" else "arm"):
        raise ValueError("Cross compiler target does not match {}: {}".format(arch, machine))
    for name in ("moc", "rcc", "uic", "qmake", "lrelease", "lupdate"):
        path = Path(host_tools) / name
        if not path.is_file() or not os.access(str(path), os.X_OK):
            raise ValueError("Missing executable host Qt tool: {}".format(path))
        header = elf_header(path)
        host = {"x86_64": (2, 62), "aarch64": (2, 183)}.get(platform.machine())
        if header is not None and (host is None or header[:2] != host):
            raise ValueError("Qt tool is not executable on the build host: {}".format(path))
    subprocess.run([str(Path(host_tools) / "moc"), "-v"], check=True)


def check_stage(prefix, arch, abi_file, readelf):
    prefix = Path(prefix)
    manager = prefix / "bin" / APP_ID
    payload = prefix / "share" / APP_ID / (APP_ID + ".runtime")
    check_elf(manager, arch)
    check_elf(payload, arch)
    if not (manager.stat().st_mode & 0o111) or (payload.stat().st_mode & 0o777) != 0o644:
        raise ValueError("Expected executable manager and read-only standalone payload (0644)")
    if (prefix / "bin" / (APP_ID + ".runtime")).exists():
        raise ValueError("Do not install a sibling runtime executable in private-bin")
    for path, with_cef in ((manager, True), (payload, False)):
        dynamic = subprocess.check_output([readelf, "-d", str(path)], universal_newlines=True)
        needed = [line for line in dynamic.splitlines() if "(NEEDED)" in line]
        has_cef = any("libcef.so" in line for line in needed)
        has_webview = any("libaurorawebview.so" in line for line in needed)
        if has_cef != with_cef or has_webview != with_cef:
            raise ValueError("CEF dependency separation is incorrect: {}".format(path))
    shared = prefix / "share/common/ru.androidcompatibilitylayer/appmanager/android-runtime"
    if (shared / ".standalone-abi").read_text().strip() != Path(abi_file).read_text().strip():
        raise ValueError("Staged shared runtime ABI marker does not match the manager")
    check_runtime(shared, arch)
    if arch == "aarch64" and not (shared / "app/webview/webview.apk").is_file():
        raise ValueError("The required ARM64 Android WebView APK was not staged")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("runtime", "sdk", "stage"))
    parser.add_argument("arch", choices=tuple(ARCHES))
    parser.add_argument("root")
    parser.add_argument("--host-tools")
    parser.add_argument("--cross-prefix")
    parser.add_argument("--abi-file")
    parser.add_argument("--readelf", default="readelf")
    args = parser.parse_args()
    try:
        if args.mode == "runtime":
            check_runtime(args.root, args.arch)
        elif args.mode == "sdk":
            if not args.host_tools or not args.cross_prefix:
                parser.error("sdk requires --host-tools and --cross-prefix")
            check_sdk(args.root, args.arch, args.host_tools, args.cross_prefix)
        else:
            if not args.abi_file:
                parser.error("stage requires --abi-file")
            check_stage(args.root, args.arch, args.abi_file, args.readelf)
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print("Aurora {} {} check failed: {}".format(args.arch, args.mode, error), file=sys.stderr)
        return 1
    print("Aurora {} {} inputs verified".format(args.arch, args.mode))
    return 0


if __name__ == "__main__":
    sys.exit(main())
