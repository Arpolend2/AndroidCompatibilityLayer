#!/usr/bin/env python3
"""Build the pinned private FFmpeg libraries for the Aurora media bridge."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "aurora" / "third_party" / "ffmpeg" / "ffmpeg-aurora-b545fc0.tar.gz"
PATCH = ROOT / "aurora" / "third_party" / "ffmpeg" / "droidmedia-copy.patch"
SHA256 = "84b49366a7a2a59857cb3fd46e01dfea31743fd2d61129e8f15a4ec649afb268"
MARKER = "AndroidCompatibilityLayer private FFmpeg 8\n"


def extract_source(archive, destination):
    with tarfile.open(archive, "r:gz") as source:
        for member in source:
            path = Path(member.name)
            if path.is_absolute() or ".." in path.parts or not (member.isfile() or member.isdir()):
                raise SystemExit("unsafe FFmpeg archive entry: " + member.name)
        source.extractall(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("aarch64", "arm", "x86_64"), required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--ar", required=True)
    parser.add_argument("--ranlib", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--strip", required=True)
    parser.add_argument("--sysroot", default="")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")

    output = args.output.resolve()
    # Build tools may pre-create the output directory for the marker file.
    if output.exists() and not (output.is_dir() and not any(output.iterdir())):
        marker = output / "ACL-FFMPEG8.txt"
        if not marker.is_file() or marker.read_text(encoding="ascii") != MARKER:
            raise SystemExit("refusing to replace unrecognised output: " + str(output))
    digest = hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()
    if digest != SHA256:
        raise SystemExit("FFmpeg source archive SHA-256 mismatch: " + digest)

    with tempfile.TemporaryDirectory(prefix="acl-ffmpeg8-") as temp:
        work = Path(temp)
        source = work / "source"
        source.mkdir()
        extract_source(ARCHIVE, source)
        subprocess.run(["patch", "--batch", "--forward", "-p1", "-i", str(PATCH)],
                       cwd=source, check=True)
        build = work / "build"
        build.mkdir()
        stage = work / "stage"
        prefix = stage / "usr"
        environment = os.environ.copy()
        environment.setdefault("SOURCE_DATE_EPOCH", "1794731289")
        environment["CFLAGS"] = (environment.get("CFLAGS", "") +
                                 " -ffile-prefix-map=" + str(work) + "=/usr/src/acl-ffmpeg8")
        command = [
            str(source / "configure"), "--prefix=/usr",
            "--build-suffix=-acl", "--enable-cross-compile", "--target-os=linux",
            "--arch=" + args.arch, "--cc=" + args.cc, "--ar=" + args.ar,
            "--ranlib=" + args.ranlib, "--nm=" + args.nm, "--strip=" + args.strip,
            "--disable-doc", "--disable-debug", "--disable-programs", "--disable-autodetect",
            "--disable-network", "--disable-static", "--enable-shared",
            "--disable-everything", "--disable-avdevice",
            "--disable-avfilter", "--disable-swresample", "--disable-swscale",
            "--disable-x86asm", "--enable-droidmedia",
            "--enable-decoder=h264,hevc,aac,h264_droidmedia,hevc_droidmedia",
            "--enable-demuxer=mov,matroska,mpegts,mp3,aac",
            "--enable-parser=h264,hevc,aac,mpegaudio", "--enable-protocol=file",
        ]
        if args.sysroot:
            command.append("--sysroot=" + args.sysroot)
        if args.arch == "arm":
            flags = "-march=armv7-a -mfloat-abi=hard -mfpu=vfpv3-d16"
            command.extend(["--disable-neon", "--extra-cflags=" + flags,
                            "--extra-ldflags=" + flags])
        subprocess.run(command, cwd=build, env=environment, check=True)
        subprocess.run(["make", "-j" + str(args.jobs)], cwd=build, env=environment, check=True)
        environment["DESTDIR"] = str(stage)
        subprocess.run(["make", "install"], cwd=build, env=environment, check=True)
        for path in ("include/libavcodec/avcodec.h", "include/libavutil/avconfig.h",
                     "include/libavformat/avformat.h", "lib/libavformat-acl.so.62",
                     "lib/libavcodec-acl.so.62", "lib/libavutil-acl.so.60"):
            if not (prefix / path).is_file():
                raise SystemExit("FFmpeg build did not produce " + path)
        shutil.copyfile(source / "COPYING.LGPLv2.1", prefix / "COPYING.LGPLv2.1")
        output.parent.mkdir(parents=True, exist_ok=True)
        if output.exists():
            shutil.rmtree(output)
        shutil.copytree(prefix, output, symlinks=True)
        (output / "ACL-FFMPEG8.txt").write_text(MARKER, encoding="ascii")


if __name__ == "__main__":
    main()
