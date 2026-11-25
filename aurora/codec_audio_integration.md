# Aurora MediaCodec host integration

The MediaCodec bridge builds a private FFmpeg 8 from the
vendored `aurora/third_party/ffmpeg/ffmpeg-aurora-b545fc0.tar.gz` automatically.
The source is from `https://gitflic.ru/project/timosh/ffmpeg_aurora.git`, commit
`b545fc0781c5312f867f8e548c0e24970e800a95`; its archive SHA-256 is
`84b49366a7a2a59857cb3fd46e01dfea31743fd2d61129e8f15a4ec649afb268`.
The helper verifies the hash, builds without network access or external codec
dependencies, and uses private `-acl` SONAMEs. The installed shared libraries,
corresponding complete source archive, and LGPL 2.1 license are placed in the
manager's shared organization data. The build enables LGPL 2.1 or later, not GPL.
The distributed `droidmedia-copy.patch` fixes bounds checks, chroma layout,
copy-back callback shutdown and asynchronous EOS handling in the fork. System FFmpeg
libraries are not replaced.

The RPM excludes only the private `libav*-acl.so` capabilities from generated
dependency metadata: these libraries are packaged together and loaded by path.
System-library dependencies are still generated normally.

The bridge loads the private libraries from
`Aurora::Application::organizationPathTo("appmanager/ffmpeg8")` and checks
versions and required symbols. If unavailable, it tries FFmpeg 7 **when its
headers were supplied at build time**; otherwise AAC and software video
decoding are unavailable.
MP3 remains on the bundled minimp3 host. H.264 and HEVC first try Rockchip MPP
via `dlopen("librockchip_mpp.so.1")`, then droidmedia hardware copy-back, then
software FFmpeg 8 (or FFmpeg 7 when compiled in). MPP is not a build-time
dependency; missing or incompatible symbols and initialization failures leave
the other backends available.
Before the first picture is delivered, up to 64 packets and 8 MiB are retained
for replay on the next backend if hardware fails; reaching either limit switches to
software before accepting another packet. After a picture is delivered, a
hardware failure is reported rather than replayed. A rejected input or drain
failure is reported to Java as a
`CodecException` (or `Callback.onError` in asynchronous mode), rather than
being followed by a successful EOS; further inputs are rejected until flush or
stop. Flushing does not replay lost packets or guarantee recovery. Checked
drain is registered internally without changing the public host ABI.
The droidmedia request leaves unknown frame rate and non-video audio parameters
unset with the API's negative sentinel; zero would be sent as actual metadata.
The shared path and Sailjail access for
standalone applications have not been verified on an Aurora device.

The FFmpeg path decodes H.264 and HEVC Main/Main 10 to CPU RGBA from supported YUV420P,
YUVJ420P, NV12 and 10-bit YUV420P frames. AAC decodes to one- or two-channel
interleaved signed 16-bit PCM. The bridge bounds picture dimensions and queued
output; unsupported formats and failed packets are reported. This implementation
does not support GPU surfaces, DRM or video scaling. Droidmedia copy-back
accepts only compatible 8-bit output; unsupported output triggers the same
fallback or error policy. The input admission gate waits at most five seconds
and assumes droidmedia has at least five input slots; a different third-party
implementation may still block inside its queue call. Copy-back shutdown waits
at most two seconds; if stop hangs, the
state is intentionally retained and further droidmedia openings are disabled
for the process. This does not bound third-party decoder creation or start.
Actual hardware decoding, callbacks, EOS, Sailjail access and the target
library's stop/join behavior remain unverified on an Aurora device.
The MPP backend requests timed output, handles info-change and EOS, copies
8-bit linear NV12 out of mapped MPP buffers and brackets CPU reads with
`DMA_BUF_IOCTL_SYNC` read-start/read-end. Unsupported formats, inaccessible
DMA-BUF synchronization, MPP errors and EOS without any decoded picture
trigger replay before the first picture or an error afterwards. If optional
color metadata is unavailable, the path assumes limited range and chooses
BT.601 below 720 lines, BT.709 otherwise. The NV12 frame goes through the existing CPU
BGRA conversion; there is no MPP DMA-BUF/EGLImage zero-copy presentation.
MPP 10-bit output is not handled by this path and falls back before first
picture. The public codec interface currently exposes no VP9 decoder.
The test overlay identifies the selected MPP decoder, its first frame and
specific MPP failures, and distinguishes unavailable FFmpeg/droidmedia libraries,
rejected hardware capability checks, combined create/configure failures,
start failures, queue/dequeue errors, queue/drain timeouts, rejected output
frames, asynchronous errors and the pre-output replay limit. The create call
combines creation and configuration, so its failure cannot identify either
step more precisely. These diagnostics do not identify a Sailjail denial
without additional device logs.
MediaCodec supplies a presentation timestamp but no decoding timestamp; the
FFmpeg packets retain that PTS and leave DTS unknown. For normalized Annex B
video the bridge marks packets whose first VCL NAL is an H.264 or HEVC IDR.
This conservative hint does not validate the bitstream, parse length-prefixed
packets, or establish that a stream is safe to replay.
Native `AMediaCodec` uses the same packet decoder and fallback policy. The
private `libavformat-acl.so.62` supplies compressed samples, codec
configuration, timestamps, sync flags and seeks to `AMediaExtractor` for
MP4, Matroska, MPEG-TS, AAC and MP3. The older ticket-based media host remains
available when the private demuxer cannot open a file. The packet decoder
supports H.264, HEVC, AAC and MP3; unsupported codecs and DRM are rejected.
Input access units are limited to 1 MiB; untimed demuxed packets are rejected.
Video buffers are CPU NV12 or RGBA and
SurfaceTexture presentation uses a copy, not a GPU decoder surface. Apps
with their own decoders can bypass both paths.

For FFmpeg 7 fallback, prepare matching libavcodec 61/libavutil 59 headers:

```sh
python3 tools/prepare-ffmpeg7-headers.py --arch aarch64
```

In the Aurora IDE build step, append the following to the existing quoted
**Additional arguments**, adjusting the source mount when necessary:

```text
 -DANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR:PATH=/workspace/megler-w10m/build/ffmpeg-7.1.1-aarch64-headers
```

This only compiles the fallback; the matching FFmpeg 7 runtime libraries must
also be available on the device. It is not necessary to enable FFmpeg 8.
The isolated `android_compatibility_layer_codec_media_test` target uses the
just-built private libraries and accepts the path to
`core/tests/app/data/assets`. It checks H.264, HEVC 8/10-bit, AAC and MP3,
ordering, drain, flush and picture lifetime. It does not prove device playback.
The `android_compatibility_layer_codec_packet_test` target checks PTS/DTS and
the conservative Annex B IDR hint, including ambiguous length-prefixed data.
The `android_compatibility_layer_codec_demux_test` target accepts a one-second
128x72 H.264/AAC MP4 with ten video frames and B-frames, and checks its
packets, codec configuration and seek. The `android_compatibility_layer_codec_ndk_test`
target accepts the same file and exercises the extractor and codec together,
requiring ten ordered output frames and EOS, then five frames after seek and
flush. A host test file can be generated
with:

```sh
ffmpeg -f lavfi -i testsrc2=size=128x72:rate=10 -f lavfi -i sine=frequency=440:sample_rate=48000 \
  -t 1 -c:v libx264 -g 5 -bf 2 -c:a aac test.mp4
```

The NDK test also accepts an HEVC/AAC file of the same dimensions, duration
and keyframe interval with a second argument, `hevc`. Core tests cover NDK
packet decoding, EOS, flush, errors and the legacy ticket path. These host
checks do not prove device hardware decoding.
The isolated router fallback test can be built without FFmpeg or Qt:

```sh
c++ -std=c++11 aurora/codec_media_host.cpp aurora/tests/codec_fallback_test.cpp -o codec-fallback-test
./codec-fallback-test
```

The `android_compatibility_layer_codec_mpp_test` and
`android_compatibility_layer_codec_mpp_routed_test` targets use a fake MPP
library to exercise dynamic loading, NV12 copy, EOS and fallback. They do
not verify real hardware or Sailjail access.

`python tools/test-media-codec-errors.py --output <empty-directory>` checks
the Java wrapper's synchronous and asynchronous failure handling with JVM
host doubles; it requires a JDK and does not exercise a device decoder.
With FFmpeg 7 headers configured, the
`android_compatibility_layer_codec_ffmpeg7_fallback_test` target uses a
nonexistent private directory and exercises FFmpeg 7 with the same assets.
The `android_compatibility_layer_codec_unavailable_test` target checks MP3
availability without either backend; when FFmpeg 7 is compiled in, run it with
`LD_LIBRARY_PATH` set to the build directory's `codec-test-fake` subdirectory.

The droidmedia copy, callback-shutdown and drain regression can be run on an x86_64 Linux host after the
private libraries have been built. Set `build` to the absolute CMake build
directory, then:

```sh
tmp="$(mktemp -d)"
mkdir "$tmp/src" "$tmp/build"
tar -xzf aurora/third_party/ffmpeg/ffmpeg-aurora-b545fc0.tar.gz -C "$tmp/src"
patch --batch -d "$tmp/src" -p1 < aurora/third_party/ffmpeg/droidmedia-copy.patch
(cd "$tmp/build" && "$tmp/src/configure" --disable-x86asm --disable-everything)
cc -std=gnu11 -ffunction-sections -fdata-sections -I"$tmp/build" -I"$tmp/src" \
  aurora/tests/droidmedia_copy_test.c -Wl,--gc-sections \
  -L"$build/ffmpeg8/lib" -Wl,-rpath,"$build/ffmpeg8/lib" -lavcodec-acl -lavutil-acl -lm -pthread \
  -o "$tmp/copy-test"
"$tmp/copy-test"
```

The separate `acl_droidmedia_probe` CMake target builds an instrumented FFmpeg
with distinct test-only SONAMEs. Run `acl_droidmedia_probe h264 <avc.mgau>` or
`acl_droidmedia_probe hevc <hevc.mgau>` in an environment that can load
droidmedia. It requires the complete frame count and PTS sequence, exercises
flush and stop, and logs `DROIDTEST` callback/stop counters. A quiet 500 ms
observation window does not prove that no later callback is possible, and
the probe is neither installed nor part of the production build.

For a manual Android compatibility-path check, install
`build/guest/java/media-codec-test.apk`. It is offline, landscape-locked, and
shows H.264 Surface and AAC playback results separately. Running it through
the manager exercises the Android MediaCodec-to-host path; building the APK
alone does not verify that path.

On 2026-11-21, a tablet screenshot reported by the project coordinator showed
60 H.264 frames and 94,208 AAC PCM frames played using the earlier bridge.
The user subsequently reported hearing noise; its quality and correctness
were not confirmed. This is not a test of the private FFmpeg 8 integration.
