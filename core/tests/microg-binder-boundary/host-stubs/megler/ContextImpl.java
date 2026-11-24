package megler;

import android.content.ContextWrapper;

public final class ContextImpl extends ContextWrapper {
    private ContextImpl() { super(null); }
    public static ContextImpl app() { return null; }
}
