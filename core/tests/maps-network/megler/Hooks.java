package megler;

import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;

final class Hooks {
    static final Map<String, Object> contexts = new HashMap<>();
    static String rejected;
    static Method hook0(Object context, Member method, Method callback) {
        if ((method.getDeclaringClass().getName() + "#" + method.getName()).equals(rejected)) return null;
        contexts.put(method.getDeclaringClass().getName() + "#" + method.getName(), context);
        return (Method) method;
    }

    static Object call(String owner, String name, Object... args) throws Throwable {
        Object context = contexts.get(owner + "#" + name);
        if (context == null) throw new AssertionError("missing hook " + name);
        Method callback = context.getClass().getDeclaredMethod("callback", Object[].class);
        callback.setAccessible(true);
        try {
            return callback.invoke(context, (Object) args);
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw e.getCause();
        }
    }
}
