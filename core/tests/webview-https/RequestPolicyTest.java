package megler.test.webview.https;

public final class RequestPolicyTest {
    private static void check(boolean value) {
        if (!value) throw new AssertionError("request policy failure");
    }

    public static void main(String[] args) {
        String target = RequestPolicy.normalizeAddress(" YA.RU:443/robots.txt ");
        check("https://ya.ru/robots.txt".equals(target));
        String second = RequestPolicy.normalizeAddress("www.iana.org");
        check("https://www.iana.org/".equals(second));
        check(RequestPolicy.allowInitialRequest(target, target, "GET", true, false));
        check(RequestPolicy.allowInitialRequest(second, second, "GET", true, false));
        check(!RequestPolicy.allowInitialRequest(second, target, "GET", true, false));
        check(!RequestPolicy.allowInitialRequest(target, target, "POST", true, false));
        check(!RequestPolicy.allowInitialRequest(target, target, "GET", false, false));
        check(!RequestPolicy.allowInitialRequest(target, target, "GET", true, true));
        check(!RequestPolicy.allowInitialRequest(target, "https://ya.ru/", "GET", true, false));
        check(!RequestPolicy.allowInitialRequest(null, target, "GET", true, false));
        for (String invalid : new String[] { "", "http://ya.ru/", "file:///tmp/test",
                "https://user@ya.ru/", "https://ya.ru/#fragment", "not a URL" }) {
            try {
                RequestPolicy.normalizeAddress(invalid);
                throw new AssertionError("Invalid address accepted");
            } catch (IllegalArgumentException expected) {
            }
        }
        System.out.println("HTTPS request policy regression passed");
    }
}
