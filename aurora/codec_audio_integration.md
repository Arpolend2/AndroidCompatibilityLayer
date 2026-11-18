# Aurora audio codec integration

The preview supplies `run.audio`, but without `run.codec` Megler's
`MediaCodecList` does not advertise AAC or MP3 decoders. This can prevent
playback by applications that use those codecs; it does not establish which
playback path Bandcamp uses.

Add `aurora/codec_audio.cpp` and `aurora/codec_audio.h` to the preview
target, include `codec_audio.h`, and set
`run.codec = auroraAudioCodecCallbacks()` before `megler_run`. No QObject
or codec-host instance needs to be owned by the preview. Keep the existing
PCM audio host as well.

The bridge requires **FFmpeg 7 development headers** matching libavcodec
major 61 and libavutil major 59. Add those headers to the SDK build's include
path. Aurora SDK 5.2.1.200's ARM64 sysroot contains the runtime libraries,
but does not provide matching public development headers; the bundled
libwebrtc FFmpeg headers are an older, incompatible ABI and must not be
used. A matching FFmpeg 7 development package or correctly configured FFmpeg
7 source tree is required; do not copy headers into the project without
reviewing their licensing and packaging.

The bridge links only to Qt Core. It loads `libavcodec.so.61` and
`libavutil.so.59` through `QLibrary` at runtime and checks the codec ABI and
required symbols. Make the corresponding system runtime packages available
on the device, or declare appropriate package dependencies after checking
the target's package names and redistribution rules. If the libraries or
individual decoders are absent, they are not advertised. Do not rely on
their presence merely because they exist in the SDK sysroot.

Only audio is exposed: raw AAC access units with AudioSpecificConfig, and
complete MPEG Layer III frames. Megler removes ADTS headers before calling
the host. Decoding is synchronous on the calling app thread. Output is
interleaved native-endian signed 16-bit PCM, one or two channels. Multichannel
input/output, unsupported decoder sample formats, video, DRM and automatic
conversion/resampling are not implemented. AAC profiles beyond the tested
AAC-LC depend on FFmpeg and are not verified. Failed packets and unsupported
formats are logged; a flush resets the decoder and discards queued output.

Build `aurora/tests/codec_audio_test.cpp` together with
`aurora/codec_audio.cpp`, Qt5::Core, and the same FFmpeg headers. Run it with
the path to `core/tests/app/data/assets`. It requires the FFmpeg 7 runtime
AAC and MP3 decoders and checks real encoded streams, PCM levels and
timestamps, multi-frame MP3 packets, drain, flush and malformed input. Test
actual playback and availability on the device; neither this test nor the
missing callback alone confirms Bandcamp compatibility.
