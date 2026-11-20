package android.media;
import android.graphics.Rect;
import java.nio.ByteBuffer;
public abstract class Image implements AutoCloseable {
    protected boolean mIsImageValid;
    public void setCropRect(Rect rect) {}
    public abstract int getFormat();
    public abstract int getWidth();
    public abstract int getHeight();
    public abstract long getTimestamp();
    public abstract int getTransform();
    public abstract int getScalingMode();
    public abstract int getPlaneCount();
    public abstract Plane[] getPlanes();
    public abstract void close();
    public abstract static class Plane {
        public abstract ByteBuffer getBuffer();
        public abstract int getRowStride();
        public abstract int getPixelStride();
    }
}
