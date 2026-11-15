// Opt-in keybox-backed Android attestation. The keys still live in software in this process.
package megler;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.security.keystore.KeyGenParameterSpec;
import java.io.*;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.interfaces.ECPublicKey;
import java.security.interfaces.RSAPublicKey;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.*;
import org.xmlpull.v1.XmlPullParser;
import static megler.KeyStoreProvider.der;

final class KeyboxAttestation {
    private static final byte[] ATTESTATION_OID = { 6, 10, 0x2b, 6, 1, 4, 1, (byte) 0xd6, 0x79, 2, 1, 0x11 };
    private KeyboxAttestation() {}
    private static File folder() { return new File(Native.dataDir(), "attestation"); }
    static boolean enabled() { return new File(folder(), "enabled").isFile(); }

    static final class Issuer {
        final PrivateKey key;
        final X509Certificate[] certificates;
        final Properties config;
        Issuer(PrivateKey key, X509Certificate[] certificates, Properties config) {
            this.key = key; this.certificates = certificates; this.config = config;
        }
    }

    static Issuer issuer(KeyGenParameterSpec spec, String algorithm) throws GeneralSecurityException {
        try {
            byte[] challenge = spec.getAttestationChallenge();
            if (challenge == null || challenge.length > 128) throw new GeneralSecurityException("Attestation challenge exceeds 128 bytes");
            if (spec.isStrongBoxBacked() || spec.isUserAuthenticationRequired() || spec.isUniqueIdIncluded()
                    || (spec.getAttestationIds() != null && spec.getAttestationIds().length != 0))
                throw new GeneralSecurityException("Unsupported attestation authorization");
            Properties config = new Properties();
            File settings = new File(folder(), "config.properties");
            if (settings.isFile()) {
                if (settings.length() > 16384) throw new IOException("Attestation configuration exceeds 16 KiB");
                try (Reader in = new InputStreamReader(new FileInputStream(settings), StandardCharsets.UTF_8)) { config.load(in); }
            }
            int level = number(config, "securityLevel", 0);
            if (level != 0 && level != 1) throw new GeneralSecurityException("Expected software or experimental TEE level");
            if (spec.getAttestKeyAlias() != null) {
                KeyStoreProvider.Entry entry = KeyStoreProvider.entry(spec.getAttestKeyAlias(), true);
                java.security.cert.Certificate[] chain = KeyStoreProvider.certificates(spec.getAttestKeyAlias());
                if (entry == null || !(entry.secretOrPrivate instanceof PrivateKey) || chain == null)
                    throw new GeneralSecurityException("Attestation signing alias unavailable");
                X509Certificate[] certificates = new X509Certificate[chain.length];
                for (int i = 0; i < chain.length; i++) certificates[i] = (X509Certificate) chain[i];
                validate((PrivateKey) entry.secretOrPrivate, certificates);
                return new Issuer((PrivateKey) entry.secretOrPrivate, certificates, config);
            }
            String xml = read(new File(folder(), "keybox.xml"), 1048576);
            if (xml.contains("<!DOCTYPE") || xml.contains("<!ENTITY")) throw new IOException("XML declarations are not supported");
            XmlPullParser parser = android.util.Xml.newPullParser();
            parser.setInput(new StringReader(xml));
            ArrayList<Issuer> issuers = new ArrayList<>();
            String keyAlgorithm = null, privatePem = null;
            ArrayList<X509Certificate> certificates = null;
            int expected = -1, keyboxes = 0;
            for (int event = parser.next(); event != XmlPullParser.END_DOCUMENT; event = parser.next()) {
                if (event == XmlPullParser.START_TAG) {
                    String name = parser.getName();
                    if (name.equals("Keybox") && ++keyboxes > 1) throw new IOException("One keybox expected");
                    if (name.equals("Key")) {
                        if (keyAlgorithm != null) throw new IOException("Nested key");
                        String value = parser.getAttributeValue(null, "algorithm");
                        keyAlgorithm = "ecdsa".equalsIgnoreCase(value) ? "EC" : "rsa".equalsIgnoreCase(value) ? "RSA" : null;
                        if (keyAlgorithm == null) throw new IOException("Unsupported keybox algorithm");
                        privatePem = null; certificates = new ArrayList<>(); expected = -1;
                    } else if (name.equals("PrivateKey")) {
                        if (keyAlgorithm == null || privatePem != null) throw new IOException("Unexpected private key");
                        privatePem = parser.nextText();
                    } else if (name.equals("NumberOfCertificates")) {
                        expected = Integer.parseInt(parser.nextText().trim());
                        if (expected < 1 || expected > 15) throw new IOException("Invalid chain length");
                    } else if (name.equals("Certificate")) {
                        if (certificates == null || certificates.size() == 15) throw new IOException("Unexpected certificate");
                        certificates.add((X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(
                                new ByteArrayInputStream(pem(parser.nextText(), "CERTIFICATE"))));
                    }
                } else if (event == XmlPullParser.END_TAG && parser.getName().equals("Key")) {
                    if (privatePem == null || certificates == null || certificates.size() != expected)
                        throw new IOException("Incomplete keybox entry");
                    X509Certificate[] chain = certificates.toArray(new X509Certificate[0]);
                    PrivateKey key = privateKey(keyAlgorithm, privatePem, chain[0]);
                    validate(key, chain);
                    issuers.add(new Issuer(key, chain, config));
                    if (issuers.size() > 2) throw new IOException("Too many keybox entries");
                    keyAlgorithm = null; certificates = null;
                }
            }
            if (keyboxes != 1 || issuers.isEmpty()) throw new IOException("No keybox");
            for (Issuer i : issuers) if (i.key.getAlgorithm().equalsIgnoreCase(algorithm)) return i;
            return issuers.get(0); // Android permits an attestation signer of another algorithm.
        } catch (GeneralSecurityException e) { throw e; }
        catch (Exception e) { throw new GeneralSecurityException("Invalid attestation configuration", e); }
    }

    private static String read(File file, int limit) throws IOException {
        if (file.length() > limit) throw new IOException("Attestation input too large");
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (InputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[4096]; int n;
            while ((n = in.read(buffer)) != -1) {
                if (out.size() + n > limit) throw new IOException("Attestation input too large");
                out.write(buffer, 0, n);
            }
        }
        return new String(out.toByteArray(), StandardCharsets.UTF_8);
    }

    private static byte[] pem(String text, String label) throws GeneralSecurityException {
        String start = "-----BEGIN " + label + "-----", end = "-----END " + label + "-----";
        text = text.trim();
        if (!text.startsWith(start) || !text.endsWith(end)) throw new GeneralSecurityException("Unexpected PEM type");
        String body = text.substring(start.length(), text.length() - end.length()).replaceAll("\\s", "");
        try { return java.util.Base64.getDecoder().decode(body); }
        catch (IllegalArgumentException bad) { throw new GeneralSecurityException("Invalid PEM", bad); }
    }

    private static PrivateKey privateKey(String algorithm, String text, X509Certificate cert) throws GeneralSecurityException {
        byte[] encoded;
        if (text.contains("BEGIN PRIVATE KEY")) encoded = pem(text, "PRIVATE KEY");
        else if (algorithm.equals("RSA")) {
            byte[] algorithmId = { 0x30, 13, 6, 9, 0x2a, (byte) 0x86, 0x48, (byte) 0x86, (byte) 0xf7, 13, 1, 1, 1, 5, 0 };
            encoded = der(0x30, integer(0), algorithmId, der(4, pem(text, "RSA PRIVATE KEY")));
        } else {
            // SEC1's curve is supplied by its certificate's SubjectPublicKeyInfo AlgorithmIdentifier.
            Node spki = Node.one(cert.getPublicKey().getEncoded());
            byte[] algorithmId = spki.children().get(0).encoded();
            encoded = der(0x30, integer(0), algorithmId, der(4, pem(text, "EC PRIVATE KEY")));
        }
        return KeyFactory.getInstance(algorithm).generatePrivate(new PKCS8EncodedKeySpec(encoded));
    }

    private static void validate(PrivateKey key, X509Certificate[] chain) throws GeneralSecurityException {
        if (chain.length < 1 || chain.length > 15) throw new GeneralSecurityException("Invalid issuer chain");
        for (int i = 0; i < chain.length; i++) {
            chain[i].checkValidity();
            if (chain[i].getBasicConstraints() < i) throw new GeneralSecurityException("Issuer CA/path length constraint");
            boolean[] usage = chain[i].getKeyUsage();
            if (usage != null && (usage.length <= 5 || !usage[5])) throw new GeneralSecurityException("Issuer cannot sign certificates");
            if (i + 1 < chain.length) {
                if (!chain[i].getIssuerX500Principal().equals(chain[i + 1].getSubjectX500Principal()))
                    throw new GeneralSecurityException("Issuer name mismatch");
                if (chain[i + 1].getBasicConstraints() < 0) throw new GeneralSecurityException("Issuer is not a CA");
                chain[i].verify(chain[i + 1].getPublicKey());
            }
        }
        X509Certificate root = chain[chain.length - 1];
        if (!root.getIssuerX500Principal().equals(root.getSubjectX500Principal())) throw new GeneralSecurityException("Chain has no root");
        root.verify(root.getPublicKey());
        Signature proof = Signature.getInstance(signature(key));
        byte[] challenge = new byte[32]; new SecureRandom().nextBytes(challenge);
        proof.initSign(key); proof.update(challenge); byte[] signed = proof.sign();
        proof.initVerify(chain[0].getPublicKey()); proof.update(challenge);
        if (!proof.verify(signed)) throw new GeneralSecurityException("Keybox private key does not match certificate");
    }

    static byte[][] chain(PublicKey pub, KeyGenParameterSpec spec, Issuer issuer) throws GeneralSecurityException {
        int level = number(issuer.config, "securityLevel", 0);
        byte[] application = applicationId();
        byte[] authorization = authorizations(pub, spec, issuer.config, application);
        byte[] software = tagged(709, der(4, application));
        byte[] keyDescription = der(0x30, integer(3), der(10, new byte[] { (byte) level }), integer(4),
                der(10, new byte[] { (byte) level }), der(4, spec.getAttestationChallenge()), der(4, new byte[0]),
                der(0x30, level == 0 ? authorization : software),
                der(0x30, level == 1 ? authorization : new byte[0]));
        ArrayList<byte[]> extensionList = new ArrayList<>();
        extensionList.add(der(0x30, ATTESTATION_OID, der(4, keyDescription)));
        boolean attestKey = (spec.getPurposes() & 128) != 0;
        if (attestKey && spec.getPurposes() != 128) throw new GeneralSecurityException("Attestation purpose must stand alone");
        if (attestKey) extensionList.add(der(0x30, new byte[] { 6, 3, 0x55, 0x1d, 0x13 }, der(1, new byte[] { (byte) 0xff }),
                der(4, der(0x30, der(1, new byte[] { (byte) 0xff })))));
        int usage = attestKey ? 4 : ((spec.getPurposes() & 12) != 0 ? 128 : 0) | ((spec.getPurposes() & 3) != 0 ? 32 : 0)
                | ((spec.getPurposes() & 64) != 0 ? 8 : 0);
        if (usage != 0) extensionList.add(der(0x30, new byte[] { 6, 3, 0x55, 0x1d, 0x0f }, der(1, new byte[] { (byte) 0xff }),
                der(4, der(3, new byte[] { (byte) Integer.numberOfTrailingZeros(usage), (byte) usage }))));
        byte[] extensions = der(0x30, join(extensionList));
        byte[] algorithm = signatureAlgorithm(issuer.key);
        javax.security.auth.x500.X500Principal subject = spec.getCertificateSubject() != null ? spec.getCertificateSubject()
                : new javax.security.auth.x500.X500Principal("CN=Android Keystore Key");
        BigInteger serial = spec.getCertificateSerialNumber() != null ? spec.getCertificateSerialNumber() : BigInteger.ONE;
        Date before = spec.getCertificateNotBefore() != null ? spec.getCertificateNotBefore() : new Date(0);
        Date after = spec.getCertificateNotAfter() != null ? spec.getCertificateNotAfter() : issuer.certificates[0].getNotAfter();
        if (before.after(after) || serial.signum() <= 0) throw new GeneralSecurityException("Invalid certificate validity/serial");
        byte[] tbs = der(0x30, der(0xa0, integer(2)), der(2, serial.toByteArray()), algorithm,
                issuer.certificates[0].getSubjectX500Principal().getEncoded(),
                der(0x30, KeyStoreProvider.time(before), KeyStoreProvider.time(after)), subject.getEncoded(), pub.getEncoded(), der(0xa3, extensions));
        Signature signer = Signature.getInstance(signature(issuer.key)); signer.initSign(issuer.key); signer.update(tbs);
        byte[] signed = signer.sign(), bits = new byte[signed.length + 1]; System.arraycopy(signed, 0, bits, 1, signed.length);
        byte[] leaf = der(0x30, tbs, algorithm, der(3, bits));
        X509Certificate parsed = (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(new ByteArrayInputStream(leaf));
        parsed.verify(issuer.certificates[0].getPublicKey());
        if (!Arrays.equals(pub.getEncoded(), parsed.getPublicKey().getEncoded())) throw new GeneralSecurityException("Attested public key mismatch");
        byte[][] chain = new byte[issuer.certificates.length + 1][]; chain[0] = leaf;
        for (int i = 0; i < issuer.certificates.length; i++) chain[i + 1] = issuer.certificates[i].getEncoded();
        android.util.Log.i("KeyboxAttestation", "Generated " + pub.getAlgorithm() + " chain of " + chain.length
                + "; challenge bytes " + spec.getAttestationChallenge().length + "; configured security level " + level);
        return chain;
    }

    private static byte[] applicationId() throws GeneralSecurityException {
        try {
            boolean hosted = MicroG.calledByHosted();
            Context context = hosted ? MicroG.context(GooglePackages.GMS) : AppRunner.application();
            if (context == null) throw new GeneralSecurityException("Attestation caller unavailable");
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), PackageManager.GET_SIGNATURES);
            ArrayList<byte[]> digests = new ArrayList<>();
            if (info.signatures == null || info.signatures.length == 0) throw new GeneralSecurityException("Attestation caller has no signature");
            for (android.content.pm.Signature signature : info.signatures)
                digests.add(der(4, MessageDigest.getInstance("SHA-256").digest(signature.toByteArray())));
            return der(0x30, set(Collections.singletonList(der(0x30,
                    der(4, info.packageName.getBytes(StandardCharsets.UTF_8)), integer(info.versionCode & 0xffffffffL)))), set(digests));
        } catch (PackageManager.NameNotFoundException e) { throw new GeneralSecurityException("Attestation caller missing", e); }
    }

    private static byte[] authorizations(PublicKey pub, KeyGenParameterSpec spec, Properties config, byte[] application) throws GeneralSecurityException {
        ArrayList<byte[]> fields = new ArrayList<>(), purposes = new ArrayList<>();
        int requested = spec.getPurposes();
        int[] bits = { 1, 2, 4, 8, 32, 64, 128 }, values = { 0, 1, 2, 3, 5, 6, 7 };
        for (int i = 0; i < bits.length; i++) if ((requested & bits[i]) != 0) purposes.add(integer(values[i]));
        if (purposes.isEmpty() || (requested & ~239) != 0) throw new GeneralSecurityException("Unsupported key purposes");
        fields.add(tagged(1, set(purposes)));
        boolean ec = pub instanceof ECPublicKey;
        fields.add(tagged(2, integer(ec ? 3 : 1)));
        int size = ec ? ((ECPublicKey) pub).getParams().getCurve().getField().getFieldSize() : ((RSAPublicKey) pub).getModulus().bitLength();
        fields.add(tagged(3, integer(size)));
        if (spec.getDigests() != null) {
            ArrayList<byte[]> digests = new ArrayList<>();
            for (String digest : spec.getDigests()) {
                String[] names = { "NONE", "MD5", "SHA-1", "SHA-224", "SHA-256", "SHA-384", "SHA-512" };
                int value = Arrays.asList(names).indexOf(digest);
                if (value < 0) throw new GeneralSecurityException("Unsupported attestation digest");
                digests.add(integer(value));
            }
            fields.add(tagged(5, set(digests)));
        }
        ArrayList<byte[]> paddings = new ArrayList<>();
        for (String[] list : new String[][] { spec.getEncryptionPaddings(), spec.getSignaturePaddings() }) if (list != null)
            for (String name : list) {
                int value = name.equals("NoPadding") ? 1 : name.equals("OAEPPadding") ? 2 : name.equals("PSS") ? 3
                        : name.equals("PKCS1Padding") ? 4 : name.equals("PKCS1") ? 5 : -1;
                if (value < 0) throw new GeneralSecurityException("Unsupported attestation padding");
                paddings.add(integer(value));
            }
        if (!paddings.isEmpty()) fields.add(tagged(6, set(paddings)));
        if (ec) fields.add(tagged(10, integer(size == 224 ? 0 : size == 256 ? 1 : size == 384 ? 2 : size == 521 ? 3 : -1)));
        else fields.add(tagged(200, der(2, ((RSAPublicKey) pub).getPublicExponent().toByteArray())));
        if (spec.getKeyValidityStart() != null) fields.add(tagged(400, integer(spec.getKeyValidityStart().getTime())));
        if (spec.getKeyValidityForOriginationEnd() != null) fields.add(tagged(401, integer(spec.getKeyValidityForOriginationEnd().getTime())));
        if (spec.getKeyValidityForConsumptionEnd() != null) fields.add(tagged(402, integer(spec.getKeyValidityForConsumptionEnd().getTime())));
        fields.add(tagged(503, der(5, new byte[0]))); // Auth-bound keys are refused before generation.
        fields.add(tagged(701, integer(System.currentTimeMillis())));
        fields.add(tagged(702, integer(0))); // Keymaster's GENERATED is 0, unlike KeyProperties.ORIGIN_GENERATED.
        int level = number(config, "securityLevel", 0);
        if (level == 1) {
            byte[] bootKey = hex(config.getProperty("verifiedBootKey", "")), bootHash = hex(config.getProperty("verifiedBootHash", ""));
            if (bootKey.length != 32 || bootHash.length != 32) throw new GeneralSecurityException("Explicit 32-byte boot key and hash required");
            int state = number(config, "verifiedBootState", 2);
            String locked = config.getProperty("deviceLocked", "false");
            if (state < 0 || state > 3 || (!locked.equals("true") && !locked.equals("false"))) throw new GeneralSecurityException("Invalid boot claims");
            fields.add(tagged(704, der(0x30, der(4, bootKey), der(1, new byte[] { locked.equals("true") ? (byte) 0xff : 0 }),
                    der(10, new byte[] { (byte) state }), der(4, bootHash))));
        }
        fields.add(tagged(705, integer(number(config, "osVersion", osVersion(build("RELEASE", true))))));
        fields.add(tagged(706, integer(number(config, "osPatchLevel", patch(build("SECURITY_PATCH", true), false)))));
        if (level == 0) fields.add(tagged(709, der(4, application)));
        if (spec.isDevicePropertiesAttestationIncluded()) {
            fields.add(tagged(710, bytes(build("BRAND", false)))); fields.add(tagged(711, bytes(build("DEVICE", false))));
            fields.add(tagged(712, bytes(build("PRODUCT", false)))); fields.add(tagged(716, bytes(build("MANUFACTURER", false))));
            fields.add(tagged(717, bytes(build("MODEL", false))));
        }
        if (config.containsKey("vendorPatchLevel")) fields.add(tagged(718, integer(number(config, "vendorPatchLevel", 0))));
        if (config.containsKey("bootPatchLevel")) fields.add(tagged(719, integer(number(config, "bootPatchLevel", 0))));
        // Application-id stays software-enforced, even when experimental TEE claims are configured.
        return join(fields);
    }

    private static String build(String field, boolean version) throws GeneralSecurityException {
        try { return String.valueOf((version ? android.os.Build.VERSION.class : android.os.Build.class).getField(field).get(null)); }
        catch (ReflectiveOperationException e) { throw new GeneralSecurityException("Build field unavailable", e); }
    }
    private static int number(Properties config, String name, int fallback) throws GeneralSecurityException {
        try { int value = Integer.parseInt(config.getProperty(name, String.valueOf(fallback))); if (value < 0) throw new NumberFormatException(); return value; }
        catch (NumberFormatException e) { throw new GeneralSecurityException("Invalid attestation option " + name); }
    }
    private static int osVersion(String release) {
        try { String[] p = release.split("\\."); return Integer.parseInt(p[0]) * 10000 + (p.length > 1 ? Integer.parseInt(p[1]) * 100 : 0) + (p.length > 2 ? Integer.parseInt(p[2]) : 0); }
        catch (RuntimeException e) { return 0; }
    }
    private static int patch(String patch, boolean day) {
        try { return Integer.parseInt(patch.replace("-", "").substring(0, day ? 8 : 6)); }
        catch (RuntimeException e) { return 0; }
    }
    private static byte[] hex(String value) throws GeneralSecurityException {
        if ((value.length() & 1) != 0 || value.length() > 128) throw new GeneralSecurityException("Invalid boot digest");
        byte[] b = new byte[value.length() / 2];
        for (int i = 0; i < b.length; i++) {
            int a = Character.digit(value.charAt(2 * i), 16), c = Character.digit(value.charAt(2 * i + 1), 16);
            if (a < 0 || c < 0) throw new GeneralSecurityException("Invalid boot digest"); b[i] = (byte) (a * 16 + c);
        }
        return b;
    }
    private static String signature(PrivateKey key) { return key.getAlgorithm().equalsIgnoreCase("EC") ? "SHA256withECDSA" : "SHA256withRSA"; }
    private static byte[] signatureAlgorithm(PrivateKey key) {
        return key.getAlgorithm().equalsIgnoreCase("EC") ? der(0x30, new byte[] { 6, 8, 0x2a, (byte) 0x86, 0x48, (byte) 0xce, 0x3d, 4, 3, 2 })
                : der(0x30, new byte[] { 6, 9, 0x2a, (byte) 0x86, 0x48, (byte) 0x86, (byte) 0xf7, 13, 1, 1, 0x0b, 5, 0 });
    }
    private static byte[] bytes(String value) { return der(4, value.getBytes(StandardCharsets.UTF_8)); }
    private static byte[] integer(long value) { return der(2, BigInteger.valueOf(value).toByteArray()); }
    private static byte[] join(List<byte[]> values) { ByteArrayOutputStream out = new ByteArrayOutputStream(); for (byte[] b : values) out.write(b, 0, b.length); return out.toByteArray(); }
    private static byte[] set(List<byte[]> values) {
        values = new ArrayList<>(values);
        values.sort((a, b) -> { for (int i = 0; i < Math.min(a.length, b.length); i++) { int d = (a[i] & 255) - (b[i] & 255); if (d != 0) return d; } return a.length - b.length; });
        ArrayList<byte[]> unique = new ArrayList<>(); for (byte[] b : values) if (unique.isEmpty() || !Arrays.equals(unique.get(unique.size() - 1), b)) unique.add(b);
        return der(0x31, join(unique));
    }
    private static byte[] tagged(int number, byte[] value) {
        if (number < 31) return der(0xa0 | number, value);
        byte[] body = der(0xbf, value);
        ByteArrayOutputStream out = new ByteArrayOutputStream(); out.write(0xbf);
        if (number >= 128) out.write(0x80 | (number >> 7)); out.write(number & 127);
        out.write(body, 1, body.length - 1); return out.toByteArray();
    }

    /** A bounded DER reader, used only to retain the issuer certificate's algorithm identifier. */
    private static final class Node {
        final byte[] data; final int start, content, end;
        Node(byte[] data, int start) throws GeneralSecurityException {
            this.data = data; this.start = start;
            if (start < 0 || start + 2 > data.length) throw new GeneralSecurityException("Truncated DER");
            int p = start + 1, n = data[p++] & 255;
            if ((n & 128) != 0) {
                int count = n & 127; n = 0;
                if (count == 0 || count > 3 || p + count > data.length) throw new GeneralSecurityException("Invalid DER length");
                for (int i = 0; i < count; i++) n = n * 256 + (data[p++] & 255);
            }
            content = p; end = p + n;
            if (end < p || end > data.length) throw new GeneralSecurityException("Truncated DER content");
        }
        static Node one(byte[] data) throws GeneralSecurityException { Node n = new Node(data, 0); if (n.end != data.length) throw new GeneralSecurityException("Trailing DER"); return n; }
        byte[] encoded() { return Arrays.copyOfRange(data, start, end); }
        ArrayList<Node> children() throws GeneralSecurityException { ArrayList<Node> out = new ArrayList<>(); for (int p = content; p < end;) { Node n = new Node(data, p); if (n.end > end) throw new GeneralSecurityException("DER child overflow"); out.add(n); p = n.end; } return out; }
    }
}
