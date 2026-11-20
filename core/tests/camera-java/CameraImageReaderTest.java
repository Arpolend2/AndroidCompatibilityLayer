import android.media.Image;
import android.media.ImageReader;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicInteger;

public class CameraImageReaderTest {
    private static void check(boolean value) {
        if (!value) throw new AssertionError();
    }
    private static void image(ImageReader reader, int value, long timestamp) {
        Image image = reader.acquireNextImage();
        check(image != null && image.getTimestamp() == timestamp && image.getFormat() == 256);
        check(image.getWidth() == 320 && image.getHeight() == 240 && image.getPlaneCount() == 1);
        Image.Plane[] planes = image.getPlanes();
        byte[] bytes = new byte[planes[0].getBuffer().remaining()];
        planes[0].getBuffer().get(bytes);
        check(planes.length == 1 && Arrays.equals(bytes, new byte[] {(byte) value}));
        image.close();
    }
    public static void main(String[] args) {
        ImageReader reader = ImageReader.newInstance(320, 240, 256, 1);
        AtomicInteger notifications = new AtomicInteger();
        reader.setOnImageAvailableListenerWithExecutor(r -> notifications.incrementAndGet(), Runnable::run);
        check(reader.meglerQueueJpeg(new byte[] {1}, 10));
        check(reader.meglerQueueJpeg(new byte[] {2}, 20));
        check(!reader.meglerQueueJpeg(new byte[] {3}, 30));
        check(notifications.get() == 2);
        image(reader, 1, 10);
        image(reader, 2, 20);
        check(reader.acquireNextImage() == null);
        check(reader.meglerQueueJpeg(new byte[] {4}, 40));
        Image acquired = reader.acquireNextImage();
        check(reader.meglerQueueJpeg(new byte[] {5}, 50));
        check(!reader.meglerQueueJpeg(new byte[] {6}, 60));
        acquired.close();
        image(reader, 5, 50);
        check(!reader.meglerQueueJpeg(null, 0));
        reader.close();
        check(!reader.meglerQueueJpeg(new byte[] {7}, 70));
        ImageReader yuv = ImageReader.newInstance(4, 4, 35, 1);
        check(!yuv.meglerQueueJpeg(new byte[] {1}, 0));
        byte[] data = yuv.meglerDequeue();
        check(data != null && data.length == 24);
        yuv.meglerQueue(data, 7);
        Image yuvImage = yuv.acquireNextImage();
        check(yuvImage != null && yuvImage.getPlanes().length == 3 && yuvImage.getTimestamp() == 7);
        yuvImage.close();
        yuv.close();
        System.out.println("CameraImageReaderTest OK");
    }
}
