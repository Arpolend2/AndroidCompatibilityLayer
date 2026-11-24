package megler.test.media;

import android.media.MediaCodec;
import java.io.IOException;
import java.util.Arrays;

public class VideoFramesTest {
    static void check(boolean value) {
        if (!value) throw new AssertionError();
    }

    public static void main(String[] args) throws Exception {
        Main.VideoFrames frames = new Main.VideoFrames(Arrays.asList(30L, 10L, 20L, 20L));
        check(frames.take(0, 0, 10));
        check(frames.take(0, 0, 20));
        check(frames.take(0, 0, 20));
        check(frames.take(0, 0, 30));
        check(!frames.take(MediaCodec.BUFFER_FLAG_END_OF_STREAM, 0, 30) && frames.count == 4 && frames.ended);
        frames = new Main.VideoFrames(Arrays.asList(10L, 20L));
        frames.take(0, 0, 10);
        try {
            frames.take(MediaCodec.BUFFER_FLAG_END_OF_STREAM, 0, 20);
            throw new AssertionError("empty EOS counted as a frame");
        } catch (IOException expected) {}
        frames = new Main.VideoFrames(Arrays.asList(10L, 20L));
        try {
            frames.take(0, 0, 20);
            throw new AssertionError("out-of-order PTS accepted");
        } catch (IOException expected) {}
        frames = new Main.VideoFrames(Arrays.asList(10L));
        check(frames.take(MediaCodec.BUFFER_FLAG_END_OF_STREAM, 1, 10) && frames.count == 1);
        System.out.println("Video frame accounting checks passed");
    }
}
