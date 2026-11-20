package megler.test.webview.loopback;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

public final class LoopbackServerTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static String request(int port, String line) throws Exception {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress(InetAddress.getByAddress(new byte[]{127, 0, 0, 1}), port), 3000);
            socket.setSoTimeout(3000);
            socket.getOutputStream().write((line + "\r\nHost: 127.0.0.1\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
            InputStream input = socket.getInputStream();
            ByteArrayOutputStream response = new ByteArrayOutputStream();
            byte[] buf = new byte[1024];
            int n;
            while ((n = input.read(buf)) != -1) response.write(buf, 0, n);
            return new String(response.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    public static void main(String[] args) throws Exception {
        LinkedBlockingQueue<Integer> ports = new LinkedBlockingQueue<>();
        LinkedBlockingQueue<String> events = new LinkedBlockingQueue<>();
        try (LoopbackServer server = new LoopbackServer(new LoopbackServer.Listener() {
            public void ready(int port) { ports.add(port); }
            public void event(String message) { events.add(message); }
        })) {
            server.start();
            Integer port = ports.poll(5, TimeUnit.SECONDS);
            check(port != null && port > 0, "server did not bind: " + events);
            String first = request(port, "GET /page1 HTTP/1.1");
            String second = request(port, "GET /page2 HTTP/1.1");
            check(first.startsWith("HTTP/1.1 200 OK\r\n") && first.contains("<h1>Loopback page 1</h1>")
                    && first.contains("href=\"/page2\""), "page 1");
            check(second.startsWith("HTTP/1.1 200 OK\r\n") && second.contains("<h1>Loopback page 2</h1>")
                    && second.contains("href=\"/page1\""), "page 2");
            check(first.contains("Content-Security-Policy: default-src 'none'"), "content policy");
            check(request(port, "GET /other HTTP/1.1").startsWith("HTTP/1.1 404 "), "unknown path");
            check(request(port, "POST /page1 HTTP/1.1").startsWith("HTTP/1.1 405 "), "unsupported method");
            check(events.stream().anyMatch(e -> e.equals("SERVER_REQUEST GET /page1 HTTP/1.1")), "request log");
        }
        System.out.println("loopback server regression passed");
    }
}
