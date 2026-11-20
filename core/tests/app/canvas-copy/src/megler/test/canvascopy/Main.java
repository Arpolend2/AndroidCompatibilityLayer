package megler.test.canvascopy;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.View;

public final class Main extends Activity {
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        setContentView(new Probe());
    }

    private final class Probe extends View {
        private static final int RED = 0xffdd382b, GREEN = 0xff13ae67, BLUE = 0xff254aac;
        private final Bitmap source = Bitmap.createBitmap(64, 48, Bitmap.Config.ARGB_8888);
        private final Bitmap target = Bitmap.createBitmap(144, 112, Bitmap.Config.ARGB_8888);
        private final Canvas sourceCanvas = new Canvas(source);
        private final Canvas targetCanvas = new Canvas(target);
        private final Rect sourceRect = new Rect(0, 0, 64, 48);
        private final Rect direct = new Rect(8, 8, 72, 56);
        private final Rect scaled = new Rect(8, 8, 136, 104);
        private final Paint copyPaint = new Paint();
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private String status = "Tap to run: 1:1 copy, then 2x fallback";
        private boolean running;
        private long started;
        private int directCount, scaledCount, phase;
        private int directPixel, scaledPixel;
        private final Runnable tick = new Runnable() {
            @Override public void run() { step(); }
        };

        Probe() {
            super(Main.this);
            setOnClickListener(v -> {
                if (running) return;
                started = SystemClock.uptimeMillis();
                directCount = scaledCount = phase = 0;
                directPixel = scaledPixel = BLUE;
                running = true;
                step();
            });
        }

        private void copy(Rect dest, int index) {
            sourceCanvas.drawColor(GREEN);
            paint.setColor(RED);
            sourceCanvas.drawRect(0, 0, (index & 1) == 0 ? 32 : 24, 48, paint);
            targetCanvas.drawColor(BLUE);
            targetCanvas.drawBitmap(source, sourceRect, dest, copyPaint);
        }

        private boolean verify(Rect dest) {
            int y = dest.centerY();
            return target.getPixel(dest.left + 4, y) == RED &&
                   target.getPixel(dest.right - 5, y) == GREEN &&
                   target.getPixel(0, y) == BLUE;
        }

        private void step() {
            long elapsed = SystemClock.uptimeMillis() - started;
            if (elapsed < 1000) {
                status = "Starting 1:1 copy";
            } else if (elapsed < 13000) {
                if (phase == 0) Log.i("CanvasCopyProbe", "1:1 begin");
                phase = 1;
                copy(direct, directCount++);
                status = "1:1 copy: " + directCount;
            } else if (elapsed < 18000) {
                if (phase == 1) {
                    Log.i("CanvasCopyProbe", "1:1 end");
                    directPixel = verify(direct) ? GREEN : RED;
                }
                phase = 2;
                status = "Pause between tests";
            } else if (elapsed < 30000) {
                if (phase != 3) Log.i("CanvasCopyProbe", "2x begin");
                phase = 3;
                copy(scaled, scaledCount++);
                status = "2x fallback: " + scaledCount;
            } else {
                Log.i("CanvasCopyProbe", "2x end");
                scaledPixel = verify(scaled) ? GREEN : RED;
                final boolean passed = directPixel == GREEN && scaledPixel == GREEN;
                status = (passed ? "Pixel checks passed" : "Pixel checks FAILED") + " - tap to repeat";
                Log.i("CanvasCopyProbe", status + "; 1:1=" + directCount + " 2x=" + scaledCount);
                running = false;
            }
            invalidate();
            if (running) postDelayed(tick, 80);
        }

        @Override protected void onDraw(Canvas canvas) {
            canvas.drawColor(0xff172335);
            final float unit = Math.min(getWidth() / 34f, getHeight() / 11f);
            paint.setColor(0xfff0f3f5);
            paint.setTextSize(unit);
            canvas.drawText(status, unit, 2 * unit, paint);
            canvas.drawText("1:1", unit, 5 * unit, paint);
            canvas.drawText("2x", 12 * unit, 5 * unit, paint);
            paint.setColor(directPixel);
            canvas.drawRect(unit, 6 * unit, 10 * unit, 9 * unit, paint);
            paint.setColor(scaledPixel);
            canvas.drawRect(12 * unit, 6 * unit, 21 * unit, 9 * unit, paint);
        }

        @Override protected void onDetachedFromWindow() {
            removeCallbacks(tick);
            running = false;
            super.onDetachedFromWindow();
        }
    }
}
