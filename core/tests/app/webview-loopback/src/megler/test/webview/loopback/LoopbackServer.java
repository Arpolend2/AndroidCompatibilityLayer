package megler.test.webview.loopback;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

final class LoopbackServer implements Closeable {
    interface Listener {
        void ready(int port);
        void event(String message);
    }

    private static final String PAGE_1 = page("Loopback page 1", "page2");
    private static final String PAGE_2 = page("Loopback page 2", "page1");
    private final Listener listener;
    private final Object lock = new Object();
    private boolean closed;
    private ServerSocket server;
    private Socket client;

    LoopbackServer(Listener listener) {
        this.listener = listener;
    }

    void start() {
        new Thread(this::serve, "webview-loopback-server").start();
    }

    private void serve() {
        try (ServerSocket socket = new ServerSocket()) {
            socket.bind(new InetSocketAddress(InetAddress.getByAddress(new byte[]{127, 0, 0, 1}), 0));
            synchronized (lock) {
                if (closed) return;
                server = socket;
            }
            listener.ready(socket.getLocalPort());
            while (!isClosed()) {
                Socket accepted = socket.accept();
                synchronized (lock) {
                    if (closed) {
                        accepted.close();
                        return;
                    }
                    client = accepted;
                }
                try (Socket connection = accepted) {
                    connection.setSoTimeout(3000);
                    respond(connection);
                } catch (IOException failure) {
                    if (!isClosed()) listener.event("SERVER_CLIENT_ERROR " + failure);
                } finally {
                    synchronized (lock) { client = null; }
                }
            }
        } catch (IOException | SecurityException failure) {
            if (!isClosed()) listener.event("SERVER_ERROR " + failure);
        }
    }

    private void respond(Socket socket) throws IOException {
        InputStream input = socket.getInputStream();
        ByteArrayOutputStream request = new ByteArrayOutputStream();
        int c, ending = 0;
        while (request.size() < 8192 && (c = input.read()) != -1) {
            request.write(c);
            ending = (ending << 8) | c;
            if (ending == 0x0d0a0d0a) break;
        }
        if (ending != 0x0d0a0d0a) throw new IOException("incomplete or oversized request");
        String header = new String(request.toByteArray(), StandardCharsets.US_ASCII);
        String line = header.substring(0, header.indexOf("\r\n"));
        listener.event("SERVER_REQUEST " + line);
        String[] parts = line.split(" ");
        boolean get = parts.length == 3 && "GET".equals(parts[0]);
        String path = get ? parts[1] : "";
        String body = "/page1".equals(path) ? PAGE_1 : "/page2".equals(path) ? PAGE_2 : "Not found";
        String status = !get ? "405 Method Not Allowed" :
                ("/page1".equals(path) || "/page2".equals(path)) ? "200 OK" : "404 Not Found";
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        String response = "HTTP/1.1 " + status + "\r\nContent-Type: text/html; charset=utf-8\r\n"
                + "Content-Length: " + bytes.length + "\r\nCache-Control: no-store\r\n"
                + "Content-Security-Policy: default-src 'none'\r\nConnection: close\r\n\r\n";
        OutputStream output = socket.getOutputStream();
        output.write(response.getBytes(StandardCharsets.US_ASCII));
        output.write(bytes);
        output.flush();
        listener.event("SERVER_RESPONSE " + status + " " + path);
    }

    private static String page(String title, String next) {
        return "<!doctype html><html><head><meta charset=\"utf-8\">"
                + "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                + "<title>" + title + "</title></head><body><h1>" + title + "</h1>"
                + "<p>HTTP from 127.0.0.1. No external content or JavaScript.</p>"
                + "<a href=\"/" + next + "\">Open " + next + "</a></body></html>";
    }

    private boolean isClosed() {
        synchronized (lock) { return closed; }
    }

    @Override
    public void close() throws IOException {
        synchronized (lock) {
            closed = true;
            try {
                if (client != null) client.close();
            } finally {
                if (server != null) server.close();
            }
        }
    }
}
