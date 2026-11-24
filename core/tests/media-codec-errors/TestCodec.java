import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.Looper;

public class TestCodec {
    static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    static MediaCodec make() throws Exception {
        MediaCodec c = MediaCodec.createDecoderByType("video/avc");
        c.configure(MediaFormat.createVideoFormat("video/avc", 16, 16), null, (android.media.MediaCrypto) null, 0);
        return c;
    }

    public static void main(String[] args) throws Exception {
        MediaCodec c = make();
        c.start();
        megler.MediaCodecNative.fail = true;
        try {
            c.queueInputBuffer(c.dequeueInputBuffer(0), 0, 0, 42, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
            throw new AssertionError("synchronous error missing");
        } catch (MediaCodec.CodecException expected) {}
        try {
            c.dequeueOutputBuffer(new MediaCodec.BufferInfo(), 0);
            throw new AssertionError("decoder did not remain failed");
        } catch (MediaCodec.CodecException expected) {}
        megler.MediaCodecNative.fail = false;
        c.flush();
        c.queueInputBuffer(c.dequeueInputBuffer(0), 0, 0, 43, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int output = c.dequeueOutputBuffer(info, 0);
        check(output >= 0 && info.presentationTimeUs == 43 && (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0, "EOS after flush");
        c.releaseOutputBuffer(output, false);
        c.release();

        c = make();
        Handler handler = new Handler(new Looper());
        int[] events = new int[3];
        c.setCallback(new MediaCodec.Callback() {
            public void onInputBufferAvailable(MediaCodec codec, int index) { events[0]++; }
            public void onOutputBufferAvailable(MediaCodec codec, int index, MediaCodec.BufferInfo bi) { events[1]++; }
            public void onOutputFormatChanged(MediaCodec codec, MediaFormat format) {}
            public void onError(MediaCodec codec, MediaCodec.CodecException error) { events[2]++; }
        }, handler);
        c.start();
        megler.MediaCodecNative.fail = true;
        c.queueInputBuffer(0, 0, 0, 44, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
        handler.runAll();
        check(events[0] == 0 && events[1] == 0 && events[2] == 1, "async callback, stale input and EOS");
        megler.MediaCodecNative.fail = false;
        c.flush();
        c.start();
        handler.runAll();
        check(events[0] == 8 && events[1] == 0 && events[2] == 1, "async input after flush");
        c.queueInputBuffer(0, 0, 0, 45, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
        handler.runAll();
        check(events[1] == 1, "async EOS after successful decode");
        c.release();
        System.out.println("MediaCodec error checks passed");
    }
}
