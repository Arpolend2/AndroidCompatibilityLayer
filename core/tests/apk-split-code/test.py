"""Exercise real APK inspection with code only in a feature split (Linux GCC)."""
import importlib.util
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("make_test_apk", ROOT / "tools" / "make-test-apk.py")
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)


def apk(path, package="org.example.splitcode", split=None, dex=False, abi=None, native=False,
        has_code=None):
    manifest = fixtures.Manifest()
    attributes = [("package", "s", package), ("versionCode", "i", 1)]
    if split:
        attributes.append(("split", "s", split))
    manifest.start("manifest", attributes)
    manifest.start("application", [("hasCode", "b", dex if has_code is None else has_code)])
    if not split:
        manifest.start("activity", [("name", "s", "org.example.splitcode.Main")])
        manifest.start("intent-filter", [])
        manifest.start("action", [("name", "s", "android.intent.action.MAIN")])
        manifest.end("action")
        manifest.start("category", [("name", "s", "android.intent.category.LAUNCHER")])
        manifest.end("category")
        manifest.end("intent-filter")
        manifest.end("activity")
    manifest.end("application")
    manifest.end("manifest")
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("AndroidManifest.xml", manifest.build())
        if dex:
            # Inspection checks presence, not execution; no Android runtime is started.
            archive.writestr("classes.dex", b"dex\n035\0")
        if native:
            archive.writestr("lib/{}/libfixture.so".format(abi), b"\x7fELF")


def test(manager=None):
    with tempfile.TemporaryDirectory(prefix="apk-split-code-") as temporary:
        work = Path(temporary)
        sources = [
            "core/tests/apk-split-code/main.c", "core/src/apk/mg_apk.c",
            "core/src/apk/mg_bundle.c", "core/src/apk/mg_json.c",
            "core/src/drivers/mg_driver.c", "core/src/mg_base.c",
            "core/src/host/mg_host_common.c", "core/src/host/mg_host_posix.c",
            "core/src/zip/mg_zip.c",
        ]
        inspector = work / "inspect"
        subprocess.run(
            ["gcc", "-D_GNU_SOURCE", "-std=gnu11", "-O1", "-ffunction-sections",
             "-fdata-sections", "-Wl,--gc-sections", "-I" + str(ROOT / "core/third_party/zlib")]
            + [str(ROOT / source) for source in sources]
            + ["-lz", "-lpthread", "-ldl", "-lm", "-o", str(inspector)], check=True)
        apk(work / "base.apk")
        apk(work / "code.apk", split="chrome", dex=True)
        apk(work / "resource.apk", split="config.en")
        apk(work / "foreign.apk", package="org.example.other", split="chrome", dex=True)
        apk(work / "other-base.apk", dex=True)
        apk(work / "whole.apk", dex=True)
        apk(work / "native-base.apk", abi="x86_64", native=True, has_code=True)
        apk(work / "no-code-native-base.apk", abi="x86_64", native=True)
        apk(work / "foreign-abi.apk", abi="arm64-v8a", native=True)

        def inspect(base, *splits):
            output = subprocess.check_output(
                [str(inspector), str(work / base)] + [str(work / part) for part in splits],
                text=True).strip("\n").split("\t")
            return output[0], int(output[1]), output[2], int(output[3])

        assert inspect("whole.apk") == ("java", 1, "", 0)
        assert inspect("base.apk", "code.apk", "resource.apk") == ("java", 1, "", 2)
        assert inspect("base.apk")[1] == 0
        assert inspect("base.apk", "resource.apk")[1] == 0
        assert inspect("base.apk", "foreign.apk")[1] == 0
        assert inspect("base.apk", "other-base.apk")[1] == 0
        assert inspect("code.apk")[1] == 0
        assert inspect("native-base.apk", "code.apk") == ("java", 1, "x86_64", 1)
        assert inspect("no-code-native-base.apk", "code.apk") == ("java", 1, "x86_64", 1)
        assert inspect("no-code-native-base.apk") == ("native-activity", 1, "x86_64", 0)
        assert inspect("foreign-abi.apk", "code.apk") == ("java", 0, "", 1)
        print("Split-only DEX, resource-only/foreign splits and native ABI checks: 11 cases OK")

        if manager:
            for name, base, with_code, expected in (
                ("split-code", "native-base.apk", True, True),
                ("missing-code", "native-base.apk", False, False),
                ("foreign-abi", "foreign-abi.apk", True, False),
            ):
                destination = work / name
                destination.mkdir()
                data = destination / "data"
                data.mkdir()
                bundle = destination / "input.apks"
                with zipfile.ZipFile(bundle, "w") as archive:
                    archive.write(work / base, "base.apk")
                    archive.write(work / "resource.apk", "config.en.apk")
                    if with_code:
                        archive.write(work / "code.apk", "chrome.apk")
                result = subprocess.run(
                    [manager, "--unpack-store-bundle", str(ROOT / "aurora/android-runtime"),
                     str(data), str(bundle), str(destination / "copy.apks"),
                     str(destination / "base.apk"), str(destination / "splits"), "160", "en"],
                    capture_output=True, text=True, timeout=60)
                report = json.loads(result.stdout)
                assert report["ok"] == expected, (name, result.stdout, result.stderr)
                assert (result.returncode == 0) == expected
                assert "must call setWebviewParams" not in result.stderr
                if expected:
                    assert len(report["splits"]) == 2
                    assert (destination / "splits/split_chrome.apk").read_bytes() == \
                        (work / "code.apk").read_bytes()
                print("Real store bundle importer {}: OK".format(name))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manager", help="optional built manager for store importer integration")
    test(parser.parse_args().manager)
