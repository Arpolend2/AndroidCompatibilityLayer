package megler.test.media;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

public final class Main extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "MediaCodecTest";
    private final AtomicBoolean started = new AtomicBoolean();
    private volatile boolean cancelled;
    private TextView videoStatus;
    private TextView audioStatus;

    private static final class Stream {
        String mime;
        int width, height, rate, channels;
        final List<byte[]> config = new ArrayList<byte[]>();
        final List<byte[]> samples = new ArrayList<byte[]>();
        final List<Long> times = new ArrayList<Long>();

        static Stream read(InputStream input) throws IOException {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] chunk = new byte[8192];
            for (int n; (n = input.read(chunk)) != -1;) bytes.write(chunk, 0, n);
            ByteBuffer data = ByteBuffer.wrap(bytes.toByteArray()).order(ByteOrder.LITTLE_ENDIAN);
            byte[] magic = new byte[8];
            byte[] mimeBytes = new byte[32];
            data.get(magic);
            if (magic[0] != 'M' || magic[1] != 'G' || magic[2] != 'M' || magic[3] != 'E' ||
                magic[4] != 'D' || magic[5] != 'I' || magic[6] != 'A' || magic[7] != '1')
                throw new IOException("invalid stream header");
            data.get(mimeBytes);
            int mimeSize = 0;
            while (mimeSize < mimeBytes.length && mimeBytes[mimeSize] != 0) mimeSize++;
            Stream stream = new Stream();
            stream.mime = new String(mimeBytes, 0, mimeSize, "US-ASCII");
            stream.width = data.getInt();
            stream.height = data.getInt();
            stream.rate = data.getInt();
            stream.channels = data.getInt();
            int records = data.getInt();
            if (records <= 0 || records > 10000) throw new IOException("invalid record count");
            for (int i = 0; i < records; i++) {
                int flags = data.getInt();
                int size = data.getInt();
                long time = data.getLong();
                if (size < 0 || size > data.remaining()) throw new IOException("invalid record size");
                byte[] sample = new byte[size];
                data.get(sample);
                if ((flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) stream.config.add(sample);
                else {
                    stream.samples.add(sample);
                    stream.times.add(time);
                }
            }
            if (stream.samples.isEmpty()) throw new IOException("stream has no samples");
            return stream;
        }

        MediaFormat format(boolean video) {
            MediaFormat format = video ? MediaFormat.createVideoFormat(mime, width, height)
                                       : MediaFormat.createAudioFormat(mime, rate, channels);
            for (int i = 0; i < config.size(); i++)
                format.setByteBuffer("csd-" + i, ByteBuffer.wrap(config.get(i)));
            return format;
        }
    }

    static final class VideoFrames {
        private final List<Long> expected;
        int count;
        boolean ended;

        VideoFrames(List<Long> inputTimes) {
            expected = new ArrayList<Long>(inputTimes);
            Collections.sort(expected);
        }

        boolean take(int flags, int size, long pts) throws IOException {
            boolean end = (flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
            boolean picture = !end || size > 0;
            if (ended) throw new IOException("output after H.264 end-of-stream");
            if (picture) {
                if (count >= expected.size()) throw new IOException("too many H.264 frames");
                long wanted = expected.get(count);
                if (pts != wanted) throw new IOException("H.264 frame " + count + " PTS " + pts + ", expected " + wanted);
                count++;
            }
            if (end) {
                ended = true;
                if (count != expected.size()) throw new IOException("decoded " + count + "/" + expected.size() + " frames");
            }
            return picture;
        }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setBackgroundColor(Color.BLACK);
        SurfaceView surfaceView = new SurfaceView(this);
        row.addView(surfaceView, new LinearLayout.LayoutParams(0, -1, 7));
        LinearLayout results = new LinearLayout(this);
        results.setOrientation(LinearLayout.VERTICAL);
        results.setPadding(20, 20, 20, 20);
        videoStatus = status("H.264: waiting for Surface");
        audioStatus = status("AAC: waiting");
        results.addView(videoStatus);
        results.addView(audioStatus);
        row.addView(results, new LinearLayout.LayoutParams(0, -1, 3));
        setContentView(row);
        surfaceView.getHolder().addCallback(this);
    }

    private TextView status(String text) {
        TextView view = new TextView(this);
        view.setTextColor(Color.WHITE);
        view.setTextSize(20);
        view.setText(text);
        return view;
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        if (!started.compareAndSet(false, true)) return;
        final Surface surface = holder.getSurface();
        Thread audio = new Thread(new Runnable() {
            @Override public void run() { playAudio(); }
        }, "aac-test");
        Thread video = new Thread(new Runnable() {
            @Override public void run() { playVideo(surface); }
        }, "h264-test");
        audio.start();
        video.start();
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        cancelled = true;
    }

    @Override protected void onDestroy() {
        cancelled = true;
        super.onDestroy();
    }

    private void report(TextView target, String message, boolean pass) {
        Log.i(TAG, (pass ? "PASS " : "FAIL ") + message);
        final TextView view = target;
        final String text = message;
        runOnUiThread(new Runnable() {
            @Override public void run() { view.setText(text); }
        });
    }

    private Stream readAsset(String name) throws IOException {
        InputStream input = getAssets().open(name);
        try {
            return Stream.read(input);
        } finally {
            input.close();
        }
    }

    private MediaCodec decoder(String mime, MediaFormat format, Surface surface) throws Exception {
        MediaCodec codec = MediaCodec.createDecoderByType(mime);
        codec.configure(format, surface, null, 0);
        codec.start();
        return codec;
    }

    private void playVideo(Surface surface) {
        MediaCodec codec = null;
        try {
            Stream stream = readAsset("surface-avc.mgau");
            codec = decoder(stream.mime, stream.format(true), surface);
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            long start = System.nanoTime();
            VideoFrames frames = new VideoFrames(stream.times);
            for (int k = 0; k < stream.samples.size() && !cancelled; k++) {
                int index = codec.dequeueInputBuffer(10000);
                if (index < 0) throw new IOException("H.264 input buffer timeout");
                ByteBuffer input = codec.getInputBuffer(index);
                byte[] sample = stream.samples.get(k);
                if (input == null || sample.length > input.capacity()) throw new IOException("H.264 access unit too large");
                input.clear();
                input.put(sample);
                int flags = k + 1 == stream.samples.size() ? MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0;
                codec.queueInputBuffer(index, 0, sample.length, stream.times.get(k), flags);
                drainVideo(codec, info, start, flags != 0, frames);
            }
            if (cancelled) return;
            if (!frames.ended) throw new IOException("H.264 end-of-stream not received");
            report(videoStatus, "H.264: PASS " + frames.count + " frames, " + stream.width + "x" + stream.height, true);
        } catch (Exception e) {
            report(videoStatus, "H.264: FAIL " + error(e), false);
        } finally {
            release(codec);
        }
    }

    private void drainVideo(MediaCodec codec, MediaCodec.BufferInfo info, long start, boolean untilEnd,
                            VideoFrames frames) throws Exception {
        long deadline = System.nanoTime() + 20000000000L;
        while (!cancelled) {
            int index = codec.dequeueOutputBuffer(info, untilEnd ? 10000 : 0);
            if (index == MediaCodec.INFO_TRY_AGAIN_LATER) {
                if (!untilEnd) return;
                if (System.nanoTime() >= deadline) throw new IOException("H.264 end-of-stream timeout");
            } else if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat format = codec.getOutputFormat();
                int width = format.getInteger(MediaFormat.KEY_WIDTH);
                int height = format.getInteger(MediaFormat.KEY_HEIGHT);
                if (width <= 0 || height <= 0) throw new IOException("invalid output size");
            } else if (index >= 0) {
                boolean end = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                boolean hasPicture = frames.take(info.flags, info.size, info.presentationTimeUs);
                if (hasPicture) {
                    long target = start + Math.max(0, info.presentationTimeUs) * 1000L;
                    while (!cancelled && target > System.nanoTime()) {
                        long wait = Math.min(20, (target - System.nanoTime()) / 1000000L);
                        if (wait > 0) Thread.sleep(wait);
                    }
                    codec.releaseOutputBuffer(index, true);
                } else {
                    codec.releaseOutputBuffer(index, false);
                }
                if (end) {
                    return;
                }
            }
        }
    }

    private void playAudio() {
        MediaCodec codec = null;
        AudioTrack track = null;
        try {
            Stream stream = readAsset("aac.mgau");
            if (stream.rate <= 0 || stream.channels != 2) throw new IOException("expected stereo AAC");
            codec = decoder(stream.mime, stream.format(false), null);
            int channelMask = AudioFormat.CHANNEL_OUT_STEREO;
            int minimum = AudioTrack.getMinBufferSize(stream.rate, channelMask, AudioFormat.ENCODING_PCM_16BIT);
            if (minimum <= 0) throw new IOException("AudioTrack format unavailable");
            track = new AudioTrack(AudioManager.STREAM_MUSIC, stream.rate, channelMask,
                    AudioFormat.ENCODING_PCM_16BIT, Math.max(minimum, stream.rate / 2), AudioTrack.MODE_STREAM);
            if (track.getState() != AudioTrack.STATE_INITIALIZED) throw new IOException("AudioTrack initialization failed");
            track.play();

            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            final int repeats = 4;
            long repeatUs = (long) stream.samples.size() * 1024 * 1000000L / stream.rate;
            long pcmBytes = 0;
            for (int repeat = 0; repeat < repeats && !cancelled; repeat++) {
                for (int k = 0; k < stream.samples.size(); k++) {
                    int index = codec.dequeueInputBuffer(10000);
                    if (index < 0) throw new IOException("AAC input buffer timeout");
                    ByteBuffer input = codec.getInputBuffer(index);
                    byte[] sample = stream.samples.get(k);
                    if (input == null || sample.length > input.capacity()) throw new IOException("AAC access unit too large");
                    input.clear();
                    input.put(sample);
                    long pts = stream.times.get(k) + repeat * repeatUs;
                    int flags = repeat + 1 == repeats && k + 1 == stream.samples.size()
                            ? MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0;
                    codec.queueInputBuffer(index, 0, sample.length, pts, flags);
                    pcmBytes += drainAudio(codec, track, info, flags != 0);
                }
            }
            if (cancelled) return;
            long frames = pcmBytes / (2 * stream.channels);
            if (frames < stream.rate) throw new IOException("too little decoded PCM: " + frames + " frames");
            long deadline = System.nanoTime() + 5000000000L;
            while (!cancelled && (track.getPlaybackHeadPosition() & 0xffffffffL) < frames) {
                if (System.nanoTime() >= deadline) throw new IOException("AudioTrack playback timeout");
                Thread.sleep(20);
            }
            if (!cancelled) report(audioStatus, "AAC: PASS " + frames + " PCM frames played", true);
        } catch (Exception e) {
            report(audioStatus, "AAC: FAIL " + error(e), false);
        } finally {
            if (track != null) {
                try { track.stop(); } catch (IllegalStateException ignored) {}
                track.release();
            }
            release(codec);
        }
    }

    private long drainAudio(MediaCodec codec, AudioTrack track, MediaCodec.BufferInfo info, boolean untilEnd)
            throws Exception {
        long pcmBytes = 0;
        long deadline = System.nanoTime() + 20000000000L;
        while (!cancelled) {
            int index = codec.dequeueOutputBuffer(info, untilEnd ? 10000 : 0);
            if (index == MediaCodec.INFO_TRY_AGAIN_LATER) {
                if (!untilEnd) return pcmBytes;
                if (System.nanoTime() >= deadline) throw new IOException("AAC end-of-stream timeout");
            } else if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat format = codec.getOutputFormat();
                if (format.getInteger(MediaFormat.KEY_SAMPLE_RATE) != 44100 ||
                    format.getInteger(MediaFormat.KEY_CHANNEL_COUNT) != 2)
                    throw new IOException("unexpected AAC output format: " + format);
            } else if (index >= 0) {
                boolean end = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                if (info.size > 0) {
                    ByteBuffer output = codec.getOutputBuffer(index);
                    if (output == null) throw new IOException("AAC output buffer unavailable");
                    output.position(info.offset);
                    output.limit(info.offset + info.size);
                    byte[] samples = new byte[info.size];
                    output.get(samples);
                    int written = 0;
                    while (written < samples.length && !cancelled) {
                        int count = track.write(samples, written, samples.length - written);
                        if (count <= 0) throw new IOException("AudioTrack write failed: " + count);
                        written += count;
                    }
                    pcmBytes += written;
                }
                codec.releaseOutputBuffer(index, false);
                if (end) return pcmBytes;
            }
        }
        return pcmBytes;
    }

    private static String error(Exception e) {
        String message = e.getMessage();
        return e.getClass().getSimpleName() + (message == null ? "" : ": " + message);
    }

    private static void release(MediaCodec codec) {
        if (codec == null) return;
        try { codec.stop(); } catch (IllegalStateException ignored) {}
        codec.release();
    }
}
