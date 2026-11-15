package megler.test.data;

import android.content.Context;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import java.io.*;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.ECGenParameterSpec;
import java.util.*;
import javax.security.auth.x500.X500Principal;

/** Test-owned signing keys, generated each run. No downloaded keybox or its private material is a fixture. */
final class AttestationChecks {
    private static final String OID = "1.3.6.1.4.1.11129.2.1.17";
    private static final String ALIAS = "megler-test-keybox-leaf";
    private static final byte[] CHALLENGE = { 0, 1, 2, (byte) 0xff, 4, 0, 6, 7 };
    private static byte[] der(int tag, byte[]... contents) {
        ByteArrayOutputStream out = new ByteArrayOutputStream(); int length = 0;
        for (byte[] b : contents) length += b.length;
        out.write(tag);
        if (length < 128) out.write(length);
        else if (length < 256) { out.write(0x81); out.write(length); }
        else { out.write(0x82); out.write(length >> 8); out.write(length); }
        for (byte[] b : contents) out.write(b, 0, b.length);
        return out.toByteArray();
    }
    private static byte[] integer(int n) { return der(2, BigInteger.valueOf(n).toByteArray()); }
    private static String pem(String name, byte[] bytes) {
        return "-----BEGIN " + name + "-----\n" + java.util.Base64.getMimeEncoder(64, new byte[] { 10 }).encodeToString(bytes)
                + "\n-----END " + name + "-----\n";
    }
    private static X509Certificate root(KeyPair pair) throws Exception {
        boolean ec = pair.getPublic().getAlgorithm().equals("EC");
        byte[] algorithm = ec ? new byte[] { 0x30, 10, 6, 8, 0x2a, (byte) 0x86, 0x48, (byte) 0xce, 0x3d, 4, 3, 2 }
                : new byte[] { 0x30, 13, 6, 9, 0x2a, (byte) 0x86, 0x48, (byte) 0x86, (byte) 0xf7, 13, 1, 1, 11, 5, 0 };
        byte[] name = new X500Principal("CN=Megler test attestation issuer").getEncoded();
        byte[] validity = der(0x30, der(0x17, "240101000000Z".getBytes(StandardCharsets.US_ASCII)),
                der(0x17, "360101000000Z".getBytes(StandardCharsets.US_ASCII)));
        byte[] constraints = der(0x30, new byte[] { 6, 3, 0x55, 0x1d, 0x13 }, new byte[] { 1, 1, (byte) 0xff },
                der(4, new byte[] { 0x30, 3, 1, 1, (byte) 0xff }));
        byte[] tbs = der(0x30, new byte[] { (byte) 0xa0, 3, 2, 1, 2 }, integer(7), algorithm, name, validity, name,
                pair.getPublic().getEncoded(), der(0xa3, der(0x30, constraints)));
        Signature signer = Signature.getInstance(ec ? "SHA256withECDSA" : "SHA256withRSA");
        signer.initSign(pair.getPrivate()); signer.update(tbs);
        byte[] sig = signer.sign(), bits = new byte[sig.length + 1]; System.arraycopy(sig, 0, bits, 1, sig.length);
        return (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(new ByteArrayInputStream(der(0x30, tbs, algorithm, der(3, bits))));
    }
    private static void write(File file, String text) throws Exception {
        try (FileOutputStream out = new FileOutputStream(file)) { out.write(text.getBytes(StandardCharsets.UTF_8)); }
    }
    private static String keybox(KeyPair pair, X509Certificate cert, String label, byte[] privateBytes) throws Exception {
        return "<AndroidAttestation><NumberOfKeyboxes>1</NumberOfKeyboxes><Keybox DeviceID=\"test\"><Key algorithm=\""
                + (pair.getPublic().getAlgorithm().equals("EC") ? "ecdsa" : "rsa") + "\"><PrivateKey format=\"pem\">"
                + pem(label, privateBytes) + "</PrivateKey><CertificateChain><NumberOfCertificates>1</NumberOfCertificates>"
                + "<Certificate format=\"pem\">" + pem("CERTIFICATE", cert.getEncoded())
                + "</Certificate></CertificateChain></Key></Keybox></AndroidAttestation>";
    }
    private static KeyGenParameterSpec spec(String alias, byte[] challenge) {
        return new KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_SIGN | KeyProperties.PURPOSE_VERIFY)
                .setDigests(KeyProperties.DIGEST_SHA256).setAttestationChallenge(challenge).build();
    }
    private static KeyPair generate(String algorithm, KeyGenParameterSpec spec) throws Exception {
        KeyPairGenerator g = KeyPairGenerator.getInstance(algorithm, "AndroidKeyStore"); g.initialize(spec); return g.generateKeyPair();
    }
    private static void check(boolean value, String what) { if (!value) throw new IllegalStateException(what); }
    private static void verified(java.security.cert.Certificate[] chain) throws Exception {
        check(chain != null && chain.length >= 2, "chain length");
        for (int i = 0; i < chain.length; i++) {
            X509Certificate cert = (X509Certificate) chain[i];
            cert.verify(chain[Math.min(i + 1, chain.length - 1)].getPublicKey());
            if (i + 1 < chain.length) check(cert.getIssuerX500Principal().equals(((X509Certificate) chain[i + 1]).getSubjectX500Principal()), "issuer");
        }
    }
    static String run(Context context, long run) throws Exception {
        Class<?> nativeClass = Class.forName("megler.Native");
        File folder = new File((String) nativeClass.getMethod("dataDir").invoke(null), "attestation");
        check(!folder.exists(), "test requires its own attestation folder"); check(folder.mkdir(), "attestation folder");
        File marker = new File(folder, "enabled"), xml = new File(folder, "keybox.xml"), config = new File(folder, "config.properties");
        KeyStore store = KeyStore.getInstance("AndroidKeyStore"); store.load(null);
        if (run > 1) verified(store.getCertificateChain(ALIAS));
        int cases = 0;
        try {
            // Disabled mode stays the device-without-attestation error, even when a keybox file is present.
            KeyPairGenerator authorityGen = KeyPairGenerator.getInstance("EC"); authorityGen.initialize(new ECGenParameterSpec("secp256r1"));
            KeyPair authority = authorityGen.generateKeyPair(); X509Certificate ca = root(authority); ca.verify(authority.getPublic());
            String good = keybox(authority, ca, "PRIVATE KEY", authority.getPrivate().getEncoded()); write(xml, good);
            boolean disabled = false;
            try { generate("EC", spec("megler-test-disabled", CHALLENGE)); }
            catch (ProviderException e) { disabled = e.getCause() != null && e.getCause().getClass().getName().equals("android.security.KeyStoreException"); }
            check(disabled && !store.containsAlias("megler-test-disabled"), "disabled attestation"); cases++;
            KeyPairGenerator ordinary = KeyPairGenerator.getInstance("EC", "AndroidKeyStore");
            ordinary.initialize(new KeyGenParameterSpec.Builder("megler-test-keybox-kept", 12).setDigests(KeyProperties.DIGEST_SHA256).build());
            ordinary.generateKeyPair();
            byte[] kept = store.getCertificate("megler-test-keybox-kept").getEncoded();
            disabled = false;
            try { generate("EC", spec("megler-test-keybox-kept", CHALLENGE)); } catch (ProviderException expected) { disabled = true; }
            check(disabled && Arrays.equals(kept, store.getCertificate("megler-test-keybox-kept").getEncoded()), "disabled failure preserves existing alias"); cases++;
            check(marker.createNewFile(), "enable marker");
            for (String algorithm : new String[] { "EC", "RSA" }) {
                KeyPair key = generate(algorithm, spec(ALIAS, CHALLENGE));
                java.security.cert.Certificate[] chain = store.getCertificateChain(ALIAS); verified(chain);
                X509Certificate leaf = (X509Certificate) chain[0];
                check(Arrays.equals(key.getPublic().getEncoded(), leaf.getPublicKey().getEncoded()), "generated key binding");
                Node description = Node.one(Node.one(leaf.getExtensionValue(OID)).content());
                List<Node> fields = description.children();
                check(fields.size() == 8 && fields.get(0).number() == 3 && fields.get(1).number() == 0 && fields.get(3).number() == 0, "software security levels");
                check(Arrays.equals(CHALLENGE, fields.get(4).content()) && fields.get(5).content().length == 0, "challenge and unique id");
                check(fields.get(7).children().isEmpty(), "no fabricated hardware claims");
                Node auth = fields.get(6);
                check(auth.tag(2).children().get(0).number() == (algorithm.equals("EC") ? 3 : 1), "key algorithm");
                check(auth.tag(3).children().get(0).number() == (algorithm.equals("EC") ? 256 : 2048), "key size");
                check(auth.tag(702).children().get(0).number() == 0, "generated origin");
                Node appId = Node.one(auth.tag(709).children().get(0).content());
                List<Node> app = appId.children().get(0).children().get(0).children();
                check(context.getPackageName().equals(new String(app.get(0).content(), StandardCharsets.UTF_8)), "caller package");
                byte[] digest = MessageDigest.getInstance("SHA-256").digest(context.getPackageManager().getPackageInfo(context.getPackageName(), 64).signatures[0].toByteArray());
                check(Arrays.equals(digest, appId.children().get(1).children().get(0).content()), "caller signer");
                Signature proof = Signature.getInstance(algorithm.equals("EC") ? "SHA256withECDSA" : "SHA256withRSA");
                proof.initSign((PrivateKey) store.getKey(ALIAS, null)); proof.update(CHALLENGE); byte[] signature = proof.sign();
                proof.initVerify(leaf); proof.update(CHALLENGE); check(proof.verify(signature), "key still usable"); cases++;
            }
            // SEC1 and PKCS#1 forms from native keybox files; the test owns both keys.
            byte[] sec1 = Node.one(authority.getPrivate().getEncoded()).children().get(2).content();
            write(xml, keybox(authority, ca, "EC PRIVATE KEY", sec1));
            generate("EC", spec(ALIAS, new byte[0])); verified(store.getCertificateChain(ALIAS)); cases++;
            KeyPairGenerator rsa = KeyPairGenerator.getInstance("RSA"); rsa.initialize(2048); KeyPair rsaAuthority = rsa.generateKeyPair();
            X509Certificate rsaCa = root(rsaAuthority);
            byte[] pkcs1 = Node.one(rsaAuthority.getPrivate().getEncoded()).children().get(2).content();
            write(xml, keybox(rsaAuthority, rsaCa, "RSA PRIVATE KEY", pkcs1));
            generate("RSA", spec(ALIAS, CHALLENGE)); verified(store.getCertificateChain(ALIAS)); cases++;
            // A mismatched signer must not replace an existing alias.
            byte[] previous = store.getCertificate(ALIAS).getEncoded();
            write(xml, keybox(rsaAuthority, ca, "RSA PRIVATE KEY", pkcs1));
            boolean refused = false;
            try { generate("RSA", spec(ALIAS, CHALLENGE)); } catch (ProviderException expected) { refused = true; }
            check(refused && Arrays.equals(previous, store.getCertificate(ALIAS).getEncoded()), "mismatched key rollback"); cases++;
            write(xml, "<!DOCTYPE test [<!ENTITY file SYSTEM \"file:///must-not-read\">]><AndroidAttestation>&file;</AndroidAttestation>");
            refused = false;
            try { generate("EC", spec(ALIAS, CHALLENGE)); } catch (ProviderException expected) { refused = true; }
            check(refused && Arrays.equals(previous, store.getCertificate(ALIAS).getEncoded()), "XML entity rejected atomically"); cases++;
            write(xml, good);
            refused = false;
            try { generate("EC", spec(ALIAS, new byte[129])); } catch (ProviderException expected) { refused = true; }
            check(refused && Arrays.equals(previous, store.getCertificate(ALIAS).getEncoded()), "oversize challenge rejected atomically"); cases++;
            KeyGenParameterSpec strong = new KeyGenParameterSpec.Builder(ALIAS, 12).setDigests(KeyProperties.DIGEST_SHA256)
                    .setAttestationChallenge(CHALLENGE).setIsStrongBoxBacked(true).build();
            refused = false;
            try { generate("EC", strong); } catch (ProviderException expected) { refused = true; }
            check(refused && Arrays.equals(previous, store.getCertificate(ALIAS).getEncoded()), "StrongBox not silently downgraded"); cases++;
            write(xml, good); write(config, "securityLevel=1\n"); refused = false;
            try { generate("EC", spec(ALIAS, CHALLENGE)); } catch (ProviderException expected) { refused = true; }
            check(refused && Arrays.equals(previous, store.getCertificate(ALIAS).getEncoded()), "incomplete claims rollback"); cases++;
            write(config, "securityLevel=1\nverifiedBootKey=" + repeat("01", 32) + "\nverifiedBootHash=" + repeat("02", 32)
                    + "\ndeviceLocked=true\nverifiedBootState=0\nosPatchLevel=202601\n");
            generate("EC", spec(ALIAS, CHALLENGE));
            Node tee = Node.one(Node.one(((X509Certificate) store.getCertificate(ALIAS)).getExtensionValue(OID)).content());
            check(tee.children().get(1).number() == 1 && tee.children().get(6).tag(709) != null, "experimental level and software app-id");
            check(tee.children().get(7).tag(704).children().get(0).children().get(1).content()[0] == (byte) 0xff, "explicit boot claims"); cases++;
            // An attestation signing alias produces an ordinary verifiable three-certificate chain.
            config.delete();
            KeyGenParameterSpec attest = new KeyGenParameterSpec.Builder("megler-test-attest-issuer", 128)
                    .setDigests(KeyProperties.DIGEST_SHA256).setAttestationChallenge(CHALLENGE).build();
            generate("EC", attest);
            KeyGenParameterSpec.Builder builder = new KeyGenParameterSpec.Builder(ALIAS, 12).setDigests(KeyProperties.DIGEST_SHA256).setAttestationChallenge(CHALLENGE);
            builder.getClass().getMethod("setAttestKeyAlias", String.class).invoke(builder, "megler-test-attest-issuer");
            generate("RSA", builder.build());
            java.security.cert.Certificate[] finalChain = store.getCertificateChain(ALIAS); verified(finalChain);
            check(finalChain.length == 3 && ((X509Certificate) finalChain[1]).getBasicConstraints() >= 0, "attestation signing alias"); cases++;
            return cases + " cases: EC/RSA, challenge, caller, PEM forms, chain signatures, rollback, explicit claims, signing alias, persistence";
        } finally {
            marker.delete(); xml.delete(); config.delete(); check(folder.delete(), "remove attestation test folder");
        }
    }
    private static String repeat(String text, int n) { StringBuilder out = new StringBuilder(); for (int i = 0; i < n; i++) out.append(text); return out.toString(); }
    private static final class Node {
        final byte[] bytes; final int tag, start, end;
        Node(byte[] bytes, int offset) {
            this.bytes = bytes; int first = bytes[offset++] & 255;
            if ((first & 31) == 31) { int n = 0, b; do { b = bytes[offset++] & 255; n = (n << 7) | (b & 127); } while ((b & 128) != 0); tag = n; }
            else tag = (first & 0xc0) == 0x80 ? first & 31 : first;
            int length = bytes[offset++] & 255;
            if ((length & 128) != 0) { int n = length & 127; length = 0; for (int i = 0; i < n; i++) length = (length << 8) | (bytes[offset++] & 255); }
            start = offset; end = offset + length;
            if (end > bytes.length) throw new IllegalStateException("Invalid test DER");
        }
        static Node one(byte[] bytes) { Node n = new Node(bytes, 0); check(n.end == bytes.length, "DER trailing data"); return n; }
        byte[] content() { return Arrays.copyOfRange(bytes, start, end); }
        int number() { return new BigInteger(content()).intValue(); }
        List<Node> children() { ArrayList<Node> out = new ArrayList<>(); for (int i = start; i < end;) { Node n = new Node(bytes, i); out.add(n); i = n.end; } return out; }
        Node tag(int tag) { for (Node n : children()) if (n.tag == tag) return n; throw new IllegalStateException("Missing authorization " + tag); }
    }
}
