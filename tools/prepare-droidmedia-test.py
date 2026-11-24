#!/usr/bin/env python3
"""Build a test-only FFmpeg with droidmedia and callback instrumentation."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ffmpeg8", root / "tools/prepare-ffmpeg8.py")
ffmpeg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ffmpeg)
trace_patch = root / "aurora/tests/droidmedia-trace.patch"
marker = "AndroidCompatibilityLayer droidmedia test FFmpeg\n"

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--arch", choices=("aarch64", "arm", "x86_64"), required=True)
for name in ("cc", "ar", "ranlib", "nm", "strip"):
    parser.add_argument("--" + name, required=True)
parser.add_argument("--sysroot", default="")
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--jobs", type=int, default=2)
args = parser.parse_args()
if args.jobs < 1:
    parser.error("--jobs must be positive")
output = args.output.resolve()
if output.exists() and (not (output / "ACL-DROIDMEDIA-TEST.txt").is_file() or
                        (output / "ACL-DROIDMEDIA-TEST.txt").read_text(encoding="ascii") != marker):
    raise SystemExit("refusing to replace unrecognised output: " + str(output))
if hashlib.sha256(ffmpeg.ARCHIVE.read_bytes()).hexdigest() != ffmpeg.SHA256:
    raise SystemExit("FFmpeg source archive SHA-256 mismatch")

with tempfile.TemporaryDirectory(prefix="acl-droidmedia-test-") as temp:
    work = Path(temp)
    source, build, stage = work / "source", work / "build", work / "stage"
    source.mkdir()
    build.mkdir()
    ffmpeg.extract_source(ffmpeg.ARCHIVE, source)
    for patch in (ffmpeg.PATCH, trace_patch):
        subprocess.run(["patch", "--batch", "--forward", "-p1", "-i", str(patch)], cwd=source, check=True)
    command = [
        str(source / "configure"), "--prefix=/usr", "--build-suffix=-acl-dmtest",
        "--enable-cross-compile", "--target-os=linux", "--arch=" + args.arch,
        "--cc=" + args.cc, "--ar=" + args.ar, "--ranlib=" + args.ranlib,
        "--nm=" + args.nm, "--strip=" + args.strip, "--disable-doc", "--disable-debug",
        "--disable-programs", "--disable-autodetect", "--disable-network", "--disable-static",
        "--enable-shared", "--disable-everything", "--disable-avdevice", "--disable-avformat",
        "--disable-avfilter", "--disable-swresample", "--disable-swscale", "--disable-x86asm",
        "--enable-droidmedia", "--enable-decoder=h264_droidmedia,hevc_droidmedia",
    ]
    if args.sysroot:
        command.append("--sysroot=" + args.sysroot)
    if args.arch == "arm":
        flags = "-march=armv7-a -mfloat-abi=hard -mfpu=vfpv3-d16"
        command.extend(["--disable-neon", "--extra-cflags=" + flags, "--extra-ldflags=" + flags])
    env = os.environ.copy()
    env.setdefault("SOURCE_DATE_EPOCH", "1794731289")
    env["CFLAGS"] = env.get("CFLAGS", "") + " -ffile-prefix-map=" + str(work) + "=/usr/src/acl-droidmedia-test"
    subprocess.run(command, cwd=build, env=env, check=True)
    subprocess.run(["make", "-j" + str(args.jobs)], cwd=build, env=env, check=True)
    env["DESTDIR"] = str(stage)
    subprocess.run(["make", "install"], cwd=build, env=env, check=True)
    prefix = stage / "usr"
    for name in ("include/libavcodec/avcodec.h", "lib/libavcodec-acl-dmtest.so.62",
                 "lib/libavutil-acl-dmtest.so.60"):
        if not (prefix / name).is_file():
            raise SystemExit("missing test library: " + name)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(prefix, output, symlinks=True)
    (output / "ACL-DROIDMEDIA-TEST.txt").write_text(marker, encoding="ascii")
