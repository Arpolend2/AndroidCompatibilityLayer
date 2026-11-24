import android.os.ParcelFileDescriptor;
import android.system.Os;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

public final class ParcelFileDescriptorSeekTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        Path file = Files.createTempFile("megler-fd-seek", ".bin");
        try {
            Files.write(file, new byte[] {10, 20, 30, 40});
            ParcelFileDescriptor descriptor = ParcelFileDescriptor.open(file.toFile(), ParcelFileDescriptor.MODE_READ_ONLY);
            try (FileInputStream input = new FileInputStream(descriptor.getFileDescriptor())) {
                Os.register(descriptor.getFileDescriptor(), input.getChannel());
                check(descriptor.seekTo(2) == 2 && input.read() == 30, "seek to middle");
                check(descriptor.seekTo(0) == 0 && input.read() == 10, "seek to beginning");
                check(descriptor.seekTo(9) == 9 && input.read() == -1, "seek beyond end");
                boolean rejected = false;
                try { descriptor.seekTo(-1); }
                catch (IOException expected) { rejected = true; }
                check(rejected, "negative offset is rejected");
            } finally { descriptor.close(); }
            boolean rejected = false;
            try { descriptor.seekTo(0); }
            catch (IOException expected) { rejected = true; }
            check(rejected, "closed descriptor is rejected");
        } finally { Files.delete(file); }
        System.out.println("ParcelFileDescriptorSeekTest: PASS");
    }
}
