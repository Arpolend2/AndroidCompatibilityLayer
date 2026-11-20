package megler.test.webview.https;

import java.net.URI;
import java.net.URISyntaxException;
import java.util.Locale;

final class RequestPolicy {
    static final String DEFAULT_URL = "https://ya.ru/robots.txt";

    private RequestPolicy() {}

    static String normalizeAddress(String text) {
        String address = text.trim();
        if (address.isEmpty()) throw new IllegalArgumentException("Enter an HTTPS address");
        if (!address.contains("://")) address = "https://" + address;
        try {
            URI uri = new URI(address).normalize();
            if (!"https".equalsIgnoreCase(uri.getScheme()) || uri.getHost() == null
                    || uri.getRawUserInfo() != null || uri.getRawFragment() != null
                    || (uri.getPort() != -1 && (uri.getPort() < 1 || uri.getPort() > 65535))) {
                throw new IllegalArgumentException("Use an HTTPS address without credentials or a fragment");
            }
            String host = uri.getHost().toLowerCase(Locale.ROOT);
            int port = uri.getPort();
            String path = uri.getRawPath();
            return new URI("https://" + host + (port == -1 || port == 443 ? "" : ":" + port)
                    + (path.isEmpty() ? "/" : path)
                    + (uri.getRawQuery() == null ? "" : "?" + uri.getRawQuery())).toASCIIString();
        } catch (URISyntaxException invalid) {
            throw new IllegalArgumentException("Invalid HTTPS address");
        }
    }

    static boolean allowInitialRequest(String target, String url, String method,
                                       boolean mainFrame, boolean redirect) {
        return target != null && mainFrame && !redirect && "GET".equals(method) && target.equals(url);
    }
}
