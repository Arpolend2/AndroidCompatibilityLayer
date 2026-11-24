package megler;

import java.lang.reflect.Field;

public final class SharedMicrogModuleOnlyTest {
    public static void main(String[] args) throws Exception {
        Field clientUid = SharedMicrogIdentity.class.getDeclaredField("clientUid");
        clientUid.setAccessible(true);
        clientUid.setInt(null, 11000);
        try {
            if (MicroG.activity(null) != null || MicroG.provider(null) != null ||
                MicroG.sendAsSystem(null))
                throw new AssertionError("Shared client must not host microG components");
            try {
                MicroG.newActivity("example.Activity");
                throw new AssertionError("Hosted activity was allowed");
            } catch (IllegalStateException expected) {
                if (!expected.getMessage().contains("forbidden")) throw expected;
            }
            if (MicroG.bindAsSystemNow(null, 0) != null || MicroG.geocoderPresent("test"))
                throw new AssertionError("Unavailable local system service was advertised");
        } finally {
            clientUid.setInt(null, -1);
        }
        System.out.println("SharedMicrogModuleOnlyTest: PASS local hosting denied");
    }
}
