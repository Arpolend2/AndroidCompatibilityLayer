#!/usr/bin/env python3
"""Prepare FFmpeg 7 public headers outside the source tree for an Aurora cross-build."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = "7.1.1"
URL = "https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz"
SHA256 = "733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"
ARCH_CONFIG = {
    "aarch64": [
        "--enable-cross-compile", "--target-os=linux", "--arch=aarch64",
        "--cc=/opt/cross/bin/aarch64-meego-linux-gnu-gcc",
        "--cross-prefix=/opt/cross/bin/aarch64-meego-linux-gnu-",
    ],
    "x86_64": ["--target-os=linux", "--arch=x86_64"],
}


def safe_extract(archive, destination):
    root = destination.resolve()
    members = archive.getmembers()
    for member in members:
        target = (root / member.name).resolve()
        if os.path.commonpath((str(root), str(target))) != str(root):
            raise SystemExit("unsafe path in FFmpeg archive: " + member.name)
        if member.issym() or member.islnk():
            link = (target.parent / member.linkname).resolve()
            if os.path.commonpath((str(root), str(link))) != str(root):
                raise SystemExit("unsafe link in FFmpeg archive: " + member.name)
    archive.extractall(destination, members=members)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=sorted(ARCH_CONFIG), default="aarch64")
    parser.add_argument("--output", type=Path,
                        help="header directory (default: build/ffmpeg-7.1.1-<arch>-headers)")
    args = parser.parse_args()

    output = args.output or ROOT / "build" / ("ffmpeg-%s-%s-headers" % (VERSION, args.arch))
    output = output.resolve()
    marker = "FFmpeg %s\narchitecture=%s\nurl=%s\nsha256=%s\n" % (
        VERSION, args.arch, URL, SHA256)
    if output.exists():
        marker_path = output / "FFmpeg-HEADERS.txt"
        if not marker_path.is_file() or marker_path.read_text(encoding="ascii") != marker:
            if any(output.iterdir()):
                raise SystemExit("output exists and was not prepared by this helper: " + str(output))

    work_root = ROOT / "build"
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ffmpeg7-headers-", dir=str(work_root)) as temp:
        work = Path(temp)
        archive_path = work / ("ffmpeg-%s.tar.xz" % VERSION)
        request = urllib.request.Request(URL, headers={"User-Agent": "megler-ffmpeg-header-helper/1"})
        with urllib.request.urlopen(request, timeout=60) as response, archive_path.open("wb") as target:
            shutil.copyfileobj(response, target)
        digest = hashlib.sha256(archive_path.read_bytes()).hexdigest()
        if digest != SHA256:
            raise SystemExit("FFmpeg archive SHA-256 mismatch: " + digest)

        extracted = work / "source"
        extracted.mkdir()
        with tarfile.open(archive_path, "r:xz") as archive:
            safe_extract(archive, extracted)
        source = extracted / ("ffmpeg-%s" % VERSION)
        configure = [str(source / "configure"), "--disable-programs", "--disable-doc",
                     "--disable-x86asm", "--disable-everything"] + ARCH_CONFIG[args.arch]
        subprocess.run(configure, cwd=str(source), check=True)

        staged = work / "headers"
        for library in ("libavcodec", "libavutil"):
            source_library = source / library
            destination_library = staged / library
            destination_library.mkdir(parents=True)
            for header in source_library.glob("*.h"):
                shutil.copyfile(header, destination_library / header.name)
        required = (
            staged / "libavcodec" / "avcodec.h",
            staged / "libavcodec" / "version_major.h",
            staged / "libavutil" / "avconfig.h",
            staged / "libavutil" / "version_major.h",
        )
        if any(not path.is_file() for path in required):
            raise SystemExit("FFmpeg configure did not produce the required public/generated headers")
        (staged / "FFmpeg-HEADERS.txt").write_text(marker, encoding="ascii")
        shutil.copyfile(source / "COPYING.LGPLv2.1", staged / "FFmpeg-LICENSE.txt")

        output.mkdir(parents=True, exist_ok=True)
        for library in ("libavcodec", "libavutil"):
            destination_library = output / library
            destination_library.mkdir(parents=True, exist_ok=True)
            for header in (staged / library).glob("*.h"):
                shutil.copyfile(header, destination_library / header.name)
        shutil.copyfile(staged / "FFmpeg-HEADERS.txt", output / "FFmpeg-HEADERS.txt")
        shutil.copyfile(staged / "FFmpeg-LICENSE.txt", output / "FFmpeg-LICENSE.txt")

    try:
        display_path = output.relative_to(ROOT)
    except ValueError:
        display_path = output
    print(display_path)


if __name__ == "__main__":
    main()
