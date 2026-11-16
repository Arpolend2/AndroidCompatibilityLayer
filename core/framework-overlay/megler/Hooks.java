package megler;

import java.lang.reflect.Member;
import java.lang.reflect.Method;

/*
 * Megler: the runtime's own method hooks (core/src/dvm/mg_dvm_hook.c, the natives behind an app's XposedBridge) for Megler's
 * diagnoses of an app's code, such as the flag cronet-netlog (CronetNetLog).
 */
final class Hooks {
    private Hooks() {}

    /**
     * From now on every call of `original` (a Method or a Constructor) is context.callback(Object[] args), `callback` being
     * that method: the call's arguments boxed, the object first for an instance method or a constructor. Hands back the
     * backup, a Method whose invoke runs the original body on the object it is given (a constructor's too); null when the
     * method could not be hooked (the log says why).
     */
    static native Method hook0(Object context, Member original, Method callback);
}
