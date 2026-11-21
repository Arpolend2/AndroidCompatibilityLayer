#!/usr/bin/env python3
"""Build the offline landscape APK that plays synthetic H.264 and AAC through MediaCodec."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "core" / "tests" / "app" / "media"


def tool(directory, name):
    candidates = [directory / (name + ".exe"), directory / name]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit("missing Android tool: " + name + " under " + str(directory))


def main():
    sdk = Path(os.environ.get("MEGLER_ANDROID_SDK", str(Path.home() / "Documents")))
    build_tools = sdk / "build-tools" / os.environ.get("MEGLER_BUILD_TOOLS_VERSION", "29.0.3")
    platform = sdk / "platforms" / os.environ.get("MEGLER_ANDROID_PLATFORM", "android-29")
    android_jar = platform / "android.jar"
    if not android_jar.is_file():
        raise SystemExit("missing Android platform android.jar: " + str(android_jar))

    jdk = Path(os.environ.get("MEGLER_JDK", ""))
    javac = (jdk / "bin" / ("javac.exe" if os.name == "nt" else "javac")) if str(jdk) else None
    java = (jdk / "bin" / ("java.exe" if os.name == "nt" else "java")) if str(jdk) else None
    if javac is None or not javac.is_file():
        found = shutil.which("javac")
        if not found:
            raise SystemExit("no javac; set MEGLER_JDK to a JDK directory")
        javac = Path(found)
        java = Path(shutil.which("java") or "")
    if java is None or not java.is_file():
        raise SystemExit("no java runtime; set MEGLER_JDK to a JDK directory")

    aapt2 = tool(build_tools, "aapt2")
    d8 = build_tools / "lib" / "d8.jar"
    dx = tool(build_tools, "dx") if not d8.is_file() else None
    output = ROOT / "build" / "guest" / "java" / "media-codec-test.apk"
    output.parent.mkdir(parents=True, exist_ok=True)
    work_root = ROOT / "build"
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="media-codec-test-", dir=str(work_root)) as temp:
        work = Path(temp)
        assets = work / "assets"
        assets.mkdir()
        subprocess.run([sys.executable, str(ROOT / "tools" / "media-test-streams.py"),
                        "--surface", str(assets)], check=True)
        shutil.copyfile(ROOT / "core" / "tests" / "app" / "data" / "assets" / "aac.mgau",
                        assets / "aac.mgau")

        base_apk = work / "base.apk"
        subprocess.run([str(aapt2), "link", "-o", str(base_apk), "-I", str(android_jar),
                        "--manifest", str(APP / "AndroidManifest.xml"), "-A", str(assets),
                        "--min-sdk-version", "21", "--target-sdk-version", "28"], check=True)

        classes = work / "classes"
        dex = work / "dex"
        classes.mkdir()
        dex.mkdir()
        subprocess.run([str(javac), "-source", "7", "-target", "7", "-encoding", "UTF-8",
                        "-bootclasspath", str(android_jar), "-d", str(classes),
                        str(APP / "src" / "megler" / "test" / "media" / "Main.java")], check=True)
        class_jar = work / "classes.jar"
        with zipfile.ZipFile(class_jar, "w", zipfile.ZIP_DEFLATED) as jar:
            for class_file in classes.rglob("*.class"):
                jar.write(class_file, class_file.relative_to(classes).as_posix())
        if d8.is_file():
            subprocess.run([str(java), "-cp", str(d8), "com.android.tools.r8.D8", "--release",
                            "--min-api", "21", "--lib", str(android_jar), "--output", str(dex),
                            str(class_jar)], check=True)
            shutil.copyfile(dex / "classes.dex", work / "classes.dex")
        else:
            subprocess.run([str(dx), "--dex", "--output=" + str(work / "classes.dex"),
                            str(class_jar)], check=True)

        shutil.copyfile(base_apk, output)
        with zipfile.ZipFile(output, "a", zipfile.ZIP_DEFLATED) as apk:
            apk.write(work / "classes.dex", "classes.dex")

    print(output)


if __name__ == "__main__":
    main()
