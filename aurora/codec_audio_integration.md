# Aurora MediaCodec host integration

When `ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR` is set, the production
runtime compiles the FFmpeg host and a dispatcher into `acl_runtime_objects`;
`preview.cpp` routes AAC/H.264 to FFmpeg and keeps MP3 on the existing
minimp3 host. With the variable empty, the normal build remains MP3-only.
AAC/H.264 are advertised only when the matching FFmpeg runtime libraries and
the requested decoder are available.

H.264 output is software-decoded by FFmpeg and converted to CPU RGBA from
8-bit YUV420P, YUVJ420P or NV12 frames. Frames are bounded to 8192 pixels per
side, 33,554,432 pixels total, and 64 MiB of queued RGBA output. Other pixel
formats, H.265, GPU surfaces, hardware decoding, DRM and automatic video
scaling are not implemented. AAC output is interleaved native-endian signed
16-bit PCM, one or two channels. MP3 support in this FFmpeg host is retained;
the currently connected preview continues using its separate minimp3 host.

The bridge requires **FFmpeg 7 development headers** matching libavcodec
major 61 and libavutil major 59. Aurora SDK 5.2.1.200's ARM64 sysroot contains
the runtime libraries, but no matching public development headers; the
bundled libwebrtc FFmpeg headers are an older, incompatible ABI and must not
be used. Aurora's public Conan documentation describes prebuilt FFmpeg
packages and gives [6.0.1 as an example recipe](https://developer.auroraos.ru/doc/extended/conan/conan_libraries);
that does not match the FFmpeg 7 runtime ABI in this SDK.

The reproducible helper downloads the [official FFmpeg 7.1.1 release](https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz),
checks its pinned SHA-256, runs `configure` with the selected target
architecture to generate `libavutil/avconfig.h`, and copies only public
headers plus the license into a build directory outside Git:

```sh
python3 tools/prepare-ffmpeg7-headers.py --arch aarch64
```

In the current Windows SDK setup, the project is mounted in the build
environment as `/workspace/megler-w10m`. In the ARM64 apptool build step's
**Additional arguments** field, append this quoted value (including the
space after the opening quote; the IDE adds `--extra-options` itself):

```text
" -DANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR:PATH=/workspace/megler-w10m/build/ffmpeg-7.1.1-aarch64-headers"
```

Keep any other required CMake options inside the same quoted value. The path
is the build environment's path, not a Windows path; adjust it if the source
mount differs. Reconfigure and build the manager as usual. To disable the
bridge in a previously configured build, pass
`" -DANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR:PATH="`; merely removing
the argument can leave its cached value in place. No network request is made
by CMake, and the helper does not add a link-time dependency on FFmpeg.

The SDK also exposes Qt VideoChat's `QVCDecoder`, but its public API is
video-only and delivers through a Qt video surface; the SDK documentation
requires calls on the GUI thread. It does not provide the combined H.264/AAC
callback contract or CPU RGBA/audio PCM outputs expected here, so using it
would require a separate integration rather than a drop-in backend.

The bridge links only to Qt Core. It loads `libavcodec.so.61` and
`libavutil.so.59` through `QLibrary` at runtime and checks the codec ABI and
required symbols. Make the corresponding system runtime packages available
on the device, or declare appropriate package dependencies after checking
the target's package names and redistribution rules. If the libraries or
individual decoders are absent, they are not advertised. Do not rely on
their presence merely because they exist in the SDK sysroot.

The host accepts raw AAC access units with AudioSpecificConfig and complete
MPEG Layer III frames; Megler removes ADTS headers before calling it. It also
accepts H.264 Annex-B access units and parameter sets. Decoding is synchronous
on the calling app thread. Unsupported decoder formats and failed packets
are reported; flush resets decoder state and discards queued output. AAC
profiles beyond tested AAC-LC depend on FFmpeg and are not verified.

The isolated regression target
`android_compatibility_layer_codec_media_test` can be built with the same
include-directory cache entry and run with the path to
`core/tests/app/data/assets`. It checks H.264 picture pixels, display order,
dimensions, AAC PCM output, interleaved AAC/H.264 instances, MP3 output,
drain and flush. It uses the SDK container's FFmpeg runtime and verifies the
composed production dispatch, but does not prove playback on an Aurora
device.

For a manual Android compatibility-path check, install
`build/guest/java/media-codec-test.apk`. It is offline, landscape-locked, and
shows H.264 Surface and AAC playback results separately. Running it through
the manager built with the bridge exercises the Android MediaCodec-to-host
path; building the APK alone does not verify that path.

On 2026-11-21, a tablet screenshot reported by the project coordinator showed
a video frame and `H.264: PASS 60 frames, 640x360`, plus
`AAC: PASS 94208 PCM frames played`. The user subsequently reported hearing
noise; its quality and correctness have not been confirmed. The manager
configuration and the device's FFmpeg runtime-package availability were not
established by this report.
