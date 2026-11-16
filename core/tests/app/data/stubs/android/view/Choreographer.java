package android.view;

/**
 * Compile-only (tools/build-test-app.sh, "stubs"): what the step vsync-callback calls of Android 13's class -- its vsync
 * callbacks and frame data, and three of its hidden members (getLooper, getFrameTimeNanos, getLastFrameTimeNanos), which
 * Megler's framework has as public ones.
 */
public final class Choreographer {
    private Choreographer() {}
    public interface FrameCallback { void doFrame(long frameTimeNanos); }
    public interface VsyncCallback { void onVsync(FrameData data); }
    public static class FrameData {
        public long getFrameTimeNanos() { return 0; }
        public FrameTimeline[] getFrameTimelines() { return null; }
        public FrameTimeline getPreferredFrameTimeline() { return null; }
    }
    public static class FrameTimeline {
        public long getVsyncId() { return 0; }
        public long getExpectedPresentationTimeNanos() { return 0; }
        public long getDeadlineNanos() { return 0; }
    }
    public static Choreographer getInstance() { return null; }
    public void postFrameCallback(FrameCallback cb) {}
    public void removeFrameCallback(FrameCallback cb) {}
    public void postVsyncCallback(VsyncCallback cb) {}
    public void removeVsyncCallback(VsyncCallback cb) {}
    public android.os.Looper getLooper() { return null; }
    public long getFrameTimeNanos() { return 0; }
    public long getLastFrameTimeNanos() { return 0; }
}
