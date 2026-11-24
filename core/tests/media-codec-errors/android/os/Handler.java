package android.os;
public class Handler {
    private final java.util.ArrayDeque<Runnable> tasks = new java.util.ArrayDeque<>();
    public Handler(Looper looper) {}
    public boolean post(Runnable task) { tasks.add(task); return true; }
    public void runAll() { while (!tasks.isEmpty()) tasks.remove().run(); }
}
