package megler.test.data;

/** The provider's stack check must affect a DroidGuard caller and leave an ordinary caller alone. */
public final class DroidGuardProfileProbe {
    public static java.security.cert.Certificate[] chain(java.security.KeyStore store, String alias) throws Exception {
        return store.getCertificateChain(alias);
    }
}
