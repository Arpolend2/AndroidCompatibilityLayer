# Aurora audio bridge integration

The preview target includes `aurora/audio_output.cpp`, `audio_output.h`,
`audio_queue.h` and `audio_format.h`, and links Qt5::Multimedia. Include
these files in the source archive.

Create one `AuroraAudioOutput` for the lifetime of the isolated preview
process and set `run.audio = audio.callbacks()` before `megler_run`. Do not
destroy it merely because the preview closes or `megler_stop()` returns:
the core's mixer currently runs for the process lifetime and can still use
the callbacks. Call `audio.shutdown()` before destroying `QGuiApplication`:
it joins the Qt audio thread while leaving the callbacks and queue alive.
Later writes are discarded at their original pace. Shutdown is permanent
and may be called more than once. Destruction is only safe after all
callbacks have stopped.
The host opens asynchronously so the Android app thread does not wait for
audio-device initialization.

Aurora requires the `Audio` permission in the desktop file for audio playback.
It also covers audio recording; this bridge implements playback only and does
not request `Microphone`. Install the updated desktop metadata and restart the
application so the sandbox is recreated with the new permission. Missing
`Audio` is a configuration defect, but a PulseAudio connection failure can
have other causes and must be checked on the device.

For diagnosis, Qt logs the selected format or reports separately that there
is no default device, no convertible supported format, or that startup failed.
It logs once when the mixer opens and writes, when it first writes nonzero
PCM, when Qt starts, and when Qt first consumes queued and nonzero source
PCM. These are milestones, not proof of audibility: a silent input may
legitimately contain only zeroes, and consumed source samples may be dropped
by resampling or cancel in mono. A backend read does not prove that the
selected device emitted sound. No samples, track names, or URLs are logged.
The existing core flag `TL_MEDIA_TRACE` separately enables AudioTrack position
statistics.

The bridge accepts native-endian, signed 16-bit stereo PCM at Megler's rate.
It checks the default Qt device's supported formats, preferring the exact
input, then a convertible nearest/preferred format, and finally other
supported PCM combinations. Output conversion supports mono or stereo,
8/16/24/32-bit signed or unsigned integer and 32/64-bit floating point,
either byte order, from 4000 to 384000 Hz. Mono averages the channels; rate
conversion uses nearest-sample hold without anti-alias filtering, so quality
can be reduced, especially when downsampling. Unsupported combinations are
not advertised as working. The input queue holds up to 30 ms, and the bridge
requests a 20 ms Qt buffer; the backend may choose a different buffer size.
A stalled or unavailable output is logged and audio is discarded at its
original pace. There is no automatic device recovery.
The bridge deliberately leaves the optional `route` callback unset: default
system routing is used, and Android earpiece/speaker requests are not
implemented. Background media policy and capture are not implemented.

Add a test executable from `aurora/tests/audio_queue_test.cpp`, linked with
threads, and run it on a host or target with a C++11 compiler. It checks FIFO,
silence on underrun, backpressure, stalled-output fallback, and paced drops.
`aurora/tests/audio_output_test.cpp` can also be linked with
`aurora/audio_output.cpp`, Qt5::Multimedia and threads to check the callback
contract, format selection and conversion, resampling, diagnostic milestones
and worker startup/shutdown.
`aurora/tests/audio_mixer_path_test.cpp` exercises the native MediaCodec path,
standalone MP3 decoder, `mg_pcm` stream and mixer through the Qt host
callback, with a stub run config. It does not execute Java AudioTrack or
check device output. These do not replace an on-device test of Qt
Multimedia and audio routing.
