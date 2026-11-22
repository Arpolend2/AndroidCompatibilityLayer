import importlib.util
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest import mock
import zipfile


spec = importlib.util.spec_from_file_location("verify", Path(__file__).parents[1] / "verify.py")
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


def write_elf(path, arch, flags=0):
    kind, machine, _ = verify.ARCHES[arch]
    header = bytearray(64)
    header[:7] = b"\x7fELF" + bytes((kind, 1, 1))
    struct.pack_into("<H", header, 18, machine)
    struct.pack_into("<I", header, 36 if kind == 1 else 48, flags)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header)


class RuntimeAbiTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in verify.NATIVE_LIBS:
            write_elf(self.root / "lib" / name, "armv7hl")
        for name in verify.RUNTIME_DATA:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.suffix == ".apk":
                with zipfile.ZipFile(str(path), "w"):
                    pass
            else:
                path.write_bytes(b"runtime data")

    def test_complete_armv7_and_text_native_stubs(self):
        (self.root / "lib/libEGL.so").write_text("Natives are supplied by the core.\n")
        verify.check_runtime(self.root, "armv7hl")

    def test_arm64_cannot_be_reused_for_armv7(self):
        write_elf(self.root / "lib/libjavacore.so", "aarch64")
        with self.assertRaisesRegex(ValueError, "expected armv7hl ELF32"):
            verify.check_runtime(self.root, "armv7hl")

    def test_foreign_library_in_other_directory_is_rejected(self):
        write_elf(self.root / "app/native/libextra.so.1", "aarch64")
        with self.assertRaisesRegex(ValueError, "libextra.so.1"):
            verify.check_runtime(self.root, "armv7hl")

    def test_required_library_cannot_be_a_text_stub(self):
        (self.root / "lib/libjavacore.so").write_text("not a native libcore library")
        with self.assertRaisesRegex(ValueError, "libjavacore.so"):
            verify.check_runtime(self.root, "armv7hl")

    def test_native_libraries_alone_are_not_a_complete_runtime(self):
        (self.root / "framework/megler-framework.dex").unlink()
        with self.assertRaisesRegex(ValueError, "runtime is incomplete"):
            verify.check_runtime(self.root, "armv7hl")

    def test_apk_cannot_carry_foreign_native_abi(self):
        with zipfile.ZipFile(str(self.root / "webview.apk"), "w") as apk:
            apk.writestr("lib/arm64-v8a/libwebview.so", b"native")
        with self.assertRaisesRegex(ValueError, "foreign Android ABIs"):
            verify.check_runtime(self.root, "armv7hl")

    def test_universal_apk_with_matching_abi_is_allowed(self):
        with zipfile.ZipFile(str(self.root / "webview.apk"), "w") as apk:
            apk.writestr("lib/arm64-v8a/libwebview.so", b"native")
            apk.writestr("lib/armeabi-v7a/libwebview.so", b"native")
        verify.check_runtime(self.root, "armv7hl")

    def test_armv7_stage_requires_matching_webview(self):
        prefix = self.root / "stage"
        manager = prefix / "bin" / verify.APP_ID
        payload = prefix / "share" / verify.APP_ID / (verify.APP_ID + ".runtime")
        for path in (manager, payload):
            write_elf(path, "armv7hl")
        manager.chmod(0o755)
        payload.chmod(0o644)
        shared = prefix / "share/common/ru.androidcompatibilitylayer/appmanager/android-runtime"
        shutil.copytree(self.root / "lib", shared / "lib")
        shutil.copytree(self.root / "framework", shared / "framework")
        shutil.copytree(self.root / "usr", shared / "usr")
        marker = self.root / "marker"
        marker.write_text("test")
        (shared / ".standalone-abi").write_text("test")
        webview = shared / "app/webview/webview.apk"
        with mock.patch.object(verify.subprocess, "check_output",
                               side_effect=["(NEEDED) libcef.so\n(NEEDED) libaurorawebview.so", ""]):
            with self.assertRaisesRegex(ValueError, "armv7hl Android WebView APK was not staged"):
                verify.check_stage(prefix, "armv7hl", marker, "readelf")
        webview.parent.mkdir(parents=True)
        with zipfile.ZipFile(str(webview), "w") as apk:
            apk.writestr("lib/arm64-v8a/libwebview.so", b"native")
        with mock.patch.object(verify.subprocess, "check_output",
                               side_effect=["(NEEDED) libcef.so\n(NEEDED) libaurorawebview.so", ""]):
            with self.assertRaisesRegex(ValueError, "foreign Android ABIs"):
                verify.check_stage(prefix, "armv7hl", marker, "readelf")
        with zipfile.ZipFile(str(webview), "w") as apk:
            apk.writestr("lib/armeabi-v7a/libwebview.so", b"native")
        with mock.patch.object(verify.subprocess, "check_output",
                               side_effect=["(NEEDED) libcef.so\n(NEEDED) libaurorawebview.so", ""]):
            verify.check_stage(prefix, "armv7hl", marker, "readelf")

    def test_android_softfp_is_not_an_aurora_hardfloat_sdk(self):
        path = self.root / "lib/libjavacore.so"
        verify.check_elf(path, "armv7hl")
        with self.assertRaisesRegex(ValueError, "hard-float"):
            verify.check_elf(path, "armv7hl", sdk=True)
        write_elf(path, "armv7hl", 0x5000400)
        verify.check_elf(path, "armv7hl", sdk=True)

    def test_sdk_link_cannot_select_host_library(self):
        host = self.root / "host/libcef.so"
        write_elf(host, "aarch64")
        sdk = self.root / "sdk"
        (sdk / "usr/lib/cef").mkdir(parents=True)
        (sdk / "usr/lib/cef/libcef.so").symlink_to(host)
        with self.assertRaisesRegex(ValueError, "escapes the target sysroot"):
            verify.sdk_file(sdk, "/usr/lib/cef/libcef.so")
