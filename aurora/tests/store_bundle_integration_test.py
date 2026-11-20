#!/usr/bin/env python3
"""Generate fixtures with aapt/smali, then test the built manager's bundle importer.

Generate: python3 store_bundle_integration_test.py --fixtures DIR --generate --framework framework-res.apk
Test:     python3 store_bundle_integration_test.py --fixtures DIR --manager path/to/manager
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile


def generate(root, framework):
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        smali = work / "smali"
        smali.mkdir()
        (smali / "Main.smali").write_text(
            ".class public Lorg/example/splitfixture/Main;\n"
            ".super Landroid/app/Activity;\n"
            ".method public constructor <init>()V\n"
            "    .registers 1\n"
            "    invoke-direct {p0}, Landroid/app/Activity;-><init>()V\n"
            "    return-void\n"
            ".end method\n", encoding="utf-8")
        subprocess.run(["smali", "assemble", str(smali), "-o", str(work / "classes.dex")], check=True)
        for name, package, version, split in (
            ("base", "org.example.splitfixture", 7, None),
            ("split", "org.example.splitfixture", 7, "config.en"),
            ("wrong-version", "org.example.splitfixture", 8, "config.en"),
            ("wrong-package", "org.example.otherfixture", 7, "config.en"),
        ):
            manifest = work / "AndroidManifest.xml"
            split_attribute = ' split="{}"'.format(split) if split else ""
            application = (
                '<application android:hasCode="false"/>'
                if split else
                '<application android:hasCode="true"><activity android:name=".Main" android:exported="true">'
                '<intent-filter><action android:name="android.intent.action.MAIN"/>'
                '<category android:name="android.intent.category.LAUNCHER"/></intent-filter>'
                '</activity></application>'
            )
            manifest.write_text(
                '<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
                'package="{}" android:versionCode="{}"{}>{}</manifest>'.format(
                    package, version, split_attribute, application), encoding="utf-8")
            apk = root / (name + ".apk")
            subprocess.run(["aapt", "package", "-f", "-M", str(manifest), "-I", framework,
                            "-F", str(apk)], check=True)
            if not split:
                with zipfile.ZipFile(apk, "a") as archive:
                    archive.write(work / "classes.dex", "classes.dex")


def test(root, manager, session):
    base = (root / "base.apk").read_bytes()
    split = (root / "split.apk").read_bytes()
    cases = {
        "valid": [("base.apk", base), ("config.en.apk", split)],
        "wrong-version": [("base.apk", base), ("config.en.apk", (root / "wrong-version.apk").read_bytes())],
        "wrong-package": [("base.apk", base), ("config.en.apk", (root / "wrong-package.apk").read_bytes())],
        "duplicate-split": [("base.apk", base), ("config.en.apk", split), ("other.apk", split)],
        "duplicate-base": [("base.apk", base), ("other.apk", base)],
        "traversal": [("base.apk", base), ("../config.en.apk", split)],
        "too-many": [("base.apk", base)] + [("split{}.apk".format(i), split) for i in range(11)],
        "oversized": [("base.apk", base), ("config.en.apk", split)],
        "filtered": [("base.apk", base), ("config.en.apk", split)],
        "corrupt": None,
    }
    if session:
        cases["java-session"] = None
    for name, entries in cases.items():
        with tempfile.TemporaryDirectory(prefix="store-bundle-test-") as temporary:
            work = Path(temporary)
            data = work / "data"
            data.mkdir()
            source = work / "input.apks"
            if name == "java-session":
                source.write_bytes(session.read_bytes())
            elif entries is None:
                source.write_bytes(b"not a zip")
            else:
                with zipfile.ZipFile(source, "w", zipfile.ZIP_STORED) as archive:
                    for filename, contents in entries:
                        archive.writestr(filename, contents)
            if name == "oversized":
                contents = bytearray(source.read_bytes())
                first = contents.index(b"PK\x01\x02")
                second = contents.index(b"PK\x01\x02", first + 4)
                struct.pack_into("<I", contents, second + 24, 512 * 1024 * 1024 + 1)
                source.write_bytes(contents)
            command = [manager, "--unpack-store-bundle", str(work), str(data), str(source),
                       str(work / "copy.apks"), str(work / "base.apk"), str(work / "splits"),
                       "160", "ru" if name == "filtered" else "en"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60,
                                    env={**os.environ, "QT_QPA_PLATFORM": "minimal", "LC_ALL": "C"})
            if name in ("valid", "java-session"):
                assert result.returncode == 0, (name, result.stdout, result.stderr)
                report = json.loads(result.stdout)
                assert report["ok"] and report["package"] == "org.example.splitfixture"
                assert len(report["splits"]) == 1
                assert (work / "base.apk").read_bytes() == base
                assert (work / "splits" / "split_config.en.apk").read_bytes() == split
                assert not (work / "copy.apks").exists()
            else:
                assert result.returncode != 0, (name, result.stdout, result.stderr)
                if name == "filtered":
                    assert "cannot retain every split" in result.stdout, (result.stdout, result.stderr)
            print("{}: OK".format(name))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--framework")
    parser.add_argument("--manager")
    parser.add_argument("--session", type=Path, help="optional .apks produced by the Java session packer")
    args = parser.parse_args()
    if args.generate:
        if not args.framework:
            parser.error("--generate requires --framework")
        generate(args.fixtures, args.framework)
    if args.manager:
        test(args.fixtures, args.manager, args.session)
    if not args.generate and not args.manager:
        parser.error("specify --generate, --manager, or both")
