// Overlay of core/framework/gen-src/android/widget/ZoomButtonsController.java (tools/build-framework.sh).
// The generated file was the class's signatures alone: getZoomControls() and getContainer() answered null.
//
// Why. Chromium's WebView makes one of these for every WebView whose settings ask for the built-in zoom with its on-screen
// buttons (setBuiltInZoomControls(true), and setDisplayZoomControls left at its default, true), and reaches into it at
// once and at every change of the page's scroll or scale (AwZoomControls: "getZoomControls().getLayoutParams()",
// "getZoomControls().setVisibility(GONE)"). With null there, the first page such a WebView laid out threw a
// NullPointerException on the main thread out of AwContents.updateScrollState, which the WebView's native side turns into
// a fatal error: Royal Revolt 2 ended so ("illegal instruction ... in libwebviewchromium.so") when its news page came up.
//
// What is here. The controller's objects are real: a container (a FrameLayout, as Android's) holding the zoom controls
// (a ZoomControls, with FrameLayout.LayoutParams), the listener kept, and the state an app sets and asks back. The
// buttons themselves are not put on the screen: setVisible(true) is refused, isVisible() stays false and the listener is
// never told they came up. Android deprecated these buttons in API 26 in favour of the pinch, which works here; a page
// zooms by the pinch and by WebView.zoomIn()/zoomOut() as before.
package android.widget;

import android.content.Context;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

/** The on-screen zoom buttons of a view that zooms, as far as an app can tell without seeing them (see the top). */
@SuppressWarnings("deprecation")
public class ZoomButtonsController implements View.OnTouchListener {
    private final View mOwnerView;
    private FrameLayout mContainer;
    private ZoomControls mControls;
    private OnZoomListener mCallback;
    private boolean mAutoDismiss = true;

    public ZoomButtonsController(View ownerView) {
        mOwnerView = ownerView;
    }

    /* Made when first asked for, as Android's are (its getZoomControls() and getContainer() build the container): an owner
       that never asks pays nothing. Without an owner view (or one without a context) there is nothing to make them of. */
    private boolean make() {
        if (mContainer != null) return true;
        Context context = mOwnerView != null ? mOwnerView.getContext() : null;
        if (context == null) return false;
        mContainer = new FrameLayout(context);
        mContainer.setLayoutParams(new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        mControls = new ZoomControls(context);
        mContainer.addView(mControls, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
        return true;
    }

    public ViewGroup getContainer() { return make() ? mContainer : null; }
    public View getZoomControls() { return make() ? mControls : null; }

    public void setOnZoomListener(OnZoomListener listener) { mCallback = listener; }
    public void setZoomInEnabled(boolean enabled) { if (make()) mControls.setIsZoomInEnabled(enabled); }
    public void setZoomOutEnabled(boolean enabled) { if (make()) mControls.setIsZoomOutEnabled(enabled); }
    public void setZoomSpeed(long speed) { if (make()) mControls.setZoomSpeed(speed); }
    public void setFocusable(boolean focusable) {}

    public boolean isAutoDismissed() { return mAutoDismiss; }
    public void setAutoDismissed(boolean autoDismiss) { mAutoDismiss = autoDismiss; }

    /** The buttons are never on the screen here: false. */
    public boolean isVisible() { return false; }
    /** Showing the buttons is refused (nothing comes up and the listener hears nothing); hiding them finds them hidden. */
    public void setVisible(boolean visible) {}

    /** Android's is its owner view's touch listener while the buttons are up, to pass touches on to them: never, here. */
    public boolean onTouch(View v, MotionEvent event) { return false; }

    public interface OnZoomListener {
        void onVisibilityChanged(boolean visible);
        void onZoom(boolean zoomIn);
    }
}
