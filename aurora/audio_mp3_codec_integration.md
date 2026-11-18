# Standalone MP3 codec host

`audio_mp3_codec.cpp` provides `auroraMp3CodecCallbacks()`, an alternative
to the FFmpeg-backed `auroraAudioCodecCallbacks()`. It advertises MP3 only;
it does not implement AAC or video and cannot establish Bandcamp support.
The preview supplies `run.codec = auroraMp3CodecCallbacks()` alongside the
existing PCM output host. CMake includes the decoder in the application and
provides `megler_audio_mp3_codec_test`, `megler_audio_preview_codec_test`,
and `megler_audio_mixer_path_test` targets. There is only one `run.codec`;
this host is not automatically combined with the FFmpeg host.

The implementation embeds minimp3 from
https://github.com/lieff/minimp3 at commit
`6238eb88e03afc2e7064f3002ed3b8d5d3716706`, unmodified except for the
filename. `audio_minimp3_LICENSE.txt` contains its CC0 1.0 license. Include
both source and license in the source archive. The header SHA-256 is
`422a6cdcd278586af7fd1eb53e430baef75ababe62a774eccb174f7902c7dad9`.
No FFmpeg headers or runtime libraries are needed for this alternative.
The application's own distribution terms and any codec patent obligations
still need review. A successful build alone does not establish validator or
device compatibility.

Input must consist of complete MPEG Layer III frames with a specified
bitrate; free-format MP3 is rejected. Output is native-endian interleaved
16-bit mono or stereo. MPEG audio bit-reservoir dependencies may cause
initial frames after a seek or flush to produce no PCM. A failed packet
requires a flush before the decoder is used again. The output queue and
input packet size are bounded; malformed frames and queue exhaustion are
reported as errors. minimp3 may conceal some corrupt input as silence,
and is not a guarantee of detecting every malformed frame.

Build `aurora/tests/audio_mp3_codec_test.cpp` with
`aurora/audio_mp3_codec.cpp` and threads; pass
`core/tests/app/data/assets/mp3.mgau` as its argument. It checks a real
encoded tone, timestamps, combined frames, flush, malformed input, and
the PCM queue. Test MediaCodec selection, decoding, Qt output and audibility
on the actual device; none is established by this host-side test.

`aurora/tests/audio_mixer_path_test.cpp` also connects the core's real
native MediaCodec implementation to this decoder, feeds its PCM into
`mg_pcm`, and verifies that the mixer passes nonzero samples to the Aurora
output callback. It uses a stub run config and does not execute Java
AudioTrack or prove Qt backend reads or audible device output.
