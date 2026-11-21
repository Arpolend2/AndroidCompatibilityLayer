package megler.test.gmsbrokerclient;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Parcel;
import android.util.Log;
import android.widget.TextView;

public class Main extends Activity {
    private static final String GMS = "com.google.android.gms";
    private static final String BROKER = "com.google.android.gms.common.internal.IGmsServiceBroker";
    private static final String CALLBACK = "com.google.android.gms.common.internal.IGmsCallbacks";
    private TextView text;
    private boolean done;
    private int callbacks;

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private void fail(Throwable error) {
        if (done) return;
        done = true;
        Log.e("GmsBrokerTest", "FAIL", error);
        text.setText("FAIL: " + error);
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this);
        text.setText("Checking synthetic service");
        setContentView(text);
        try {
            keyChecks();
            Intent hidden = new Intent("megler.test.gmsbroker.HIDDEN").setPackage(GMS);
            check(!bindService(hidden, connection, Context.BIND_AUTO_CREATE), "hidden service must not bind");
            Intent wrong = new Intent("megler.test.gmsbroker.BIND").setPackage("megler.test.other");
            check(!bindService(wrong, connection, Context.BIND_AUTO_CREATE), "wrong package must not bind");
            Intent intent = new Intent("megler.test.gmsbroker.BIND").setPackage(GMS);
            check(bindService(intent, connection, Context.BIND_AUTO_CREATE), "service bind");
            new Handler().postDelayed(() -> { if (!done) fail(new AssertionError("binding timeout")); }, 30000);
        } catch (Throwable error) { fail(error); }
    }

    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            Parcel request = Parcel.obtain(), nested = Parcel.obtain(), reply = Parcel.obtain();
            try {
                check(GMS.equals(name.getPackageName()), "service package");
                Binder callback = new Binder() {
                    @Override protected boolean onTransact(int code, Parcel data, Parcel response, int flags) {
                        if (code != 1) return false;
                        data.enforceInterface(CALLBACK);
                        check(data.readInt() == 0 && "synthetic-result".equals(data.readString()), "callback result");
                        callbacks++;
                        response.writeNoException();
                        return true;
                    }
                };
                request.writeInterfaceToken(BROKER);
                request.writeInt(39);
                request.writeStrongBinder(new Binder());
                nested.writeStrongBinder(callback);
                request.appendFrom(nested, 0, nested.dataSize());
                check(service.transact(46, request, reply, 0), "broker transaction");
                reply.readException();
                check(callbacks == 1, "callback delivered");
                Parcel bad = Parcel.obtain(), badReply = Parcel.obtain();
                try {
                    bad.writeInterfaceToken("wrong.interface");
                    boolean rejected = false;
                    try { service.transact(46, bad, badReply, 0); }
                    catch (SecurityException expected) { rejected = true; }
                    check(rejected && callbacks == 1, "wrong interface rejected");
                } finally { bad.recycle(); badReply.recycle(); }
                unbindService(this);
                new Handler().post(() -> lifecycle());
            } catch (Throwable error) { fail(error); }
            finally { request.recycle(); nested.recycle(); reply.recycle(); }
        }
        @Override public void onServiceDisconnected(ComponentName name) { fail(new AssertionError("disconnected")); }
        @Override public void onNullBinding(ComponentName name) { fail(new AssertionError("null binding")); }
    };

    private void keyChecks() throws Exception {
        java.lang.reflect.Method method = Class.forName("megler.MicroG").getMethod("bindKey", Intent.class);
        Intent original = intent("mock:a");
        Intent extras = new Intent(original).putExtra("value", 1);
        Intent component = new Intent(original).setClassName(GMS, "megler.test.gmsbroker.Broker");
        Intent noPackage = new Intent(original).setPackage(null);
        Intent categories = new Intent(original).addCategory("a").addCategory("b");
        Intent reverse = new Intent(original).addCategory("b").addCategory("a");
        Intent emptyCategories = new Intent(original).addCategory("a");
        emptyCategories.removeCategory("a");
        check(method.invoke(null, original).equals(method.invoke(null, extras)), "extras do not change binding identity");
        check(!method.invoke(null, original).equals(method.invoke(null, component)), "component in binding identity");
        check(!method.invoke(null, original).equals(method.invoke(null, noPackage)), "package in binding identity");
        check(method.invoke(null, categories).equals(method.invoke(null, reverse)), "category order");
        check(!method.invoke(null, original).equals(method.invoke(null, emptyCategories)), "empty categories");
        Intent delimiter = new Intent("x data=mock:a");
        Intent data = new Intent("x").setData(Uri.parse("mock:a"));
        check(!method.invoke(null, delimiter).equals(method.invoke(null, data)), "binding key boundaries");
    }

    private Intent intent(String key) {
        return new Intent("megler.test.gmsbroker.BIND").setPackage(GMS).setData(Uri.parse(key));
    }

    private void state(IBinder control, String key, int binds, int unbinds, int rebinds) throws Exception {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeString(key);
            check(control.transact(47, request, reply, 0), "state transaction");
            check(reply.readInt() == binds && reply.readInt() == unbinds && reply.readInt() == rebinds,
                  "lifecycle for " + key);
        } finally { request.recycle(); reply.recycle(); }
    }

    private interface Connected { void run(ServiceConnection connection, IBinder binder) throws Exception; }
    private void bind(String key, Connected onConnected) {
        bind(intent(key), onConnected);
    }

    private void bind(Intent target, Connected onConnected) {
        ServiceConnection c = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder binder) {
                try { onConnected.run(this, binder); } catch (Throwable error) { fail(error); }
            }
            @Override public void onServiceDisconnected(ComponentName name) { fail(new AssertionError("disconnected")); }
            @Override public void onNullBinding(ComponentName name) { fail(new AssertionError("null binding")); }
        };
        check(bindService(target, c, Context.BIND_AUTO_CREATE), "bind " + target);
    }

    private void lifecycle() {
        try {
            bind("mock:control", (controlConnection, control) -> bind("mock:a", (first, original) ->
                bind("mock:a", (second, shared) -> {
                    check(original == shared, "same intent shares binder");
                    unbindService(first);
                    new Handler().post(() -> {
                        try {
                            state(control, "mock:a", 1, 0, 0);
                            unbindService(second);
                            new Handler().post(() -> {
                                try {
                                    state(control, "mock:a", 1, 1, 0);
                                    bind("mock:a", (third, rebound) -> {
                                        check(original == rebound, "rebind retains binder");
                                        state(control, "mock:a", 1, 1, 1);
                                        unbindService(third);
                                        new Handler().post(() -> {
                                            try {
                                                state(control, "mock:a", 1, 2, 1);
                                                bind("mock:c", (fourth, previous) -> {
                                                    unbindService(fourth);
                                                    new Handler().post(() -> {
                                                        try {
                                                            state(control, "mock:c", 1, 1, 0);
                                                            bind("mock:c", (fifth, fresh) -> {
                                                                check(previous != fresh, "onBind returns a new binder");
                                                                state(control, "mock:c", 2, 1, 0);
                                                                unbindService(fifth);
                                                                Intent explicit = intent("mock:control")
                                                                    .setClassName(GMS, "megler.test.gmsbroker.Broker");
                                                                bind(explicit, (different, binder) -> {
                                                                    check(binder != control, "component changes binding identity");
                                                                    unbindService(different);
                                                                    systemBinding(controlConnection, control, original);
                                                                });
                                                            });
                                                        } catch (Throwable error) { fail(error); }
                                                    });
                                                });
                                            } catch (Throwable error) { fail(error); }
                                        });
                                    });
                                } catch (Throwable error) { fail(error); }
                            });
                        } catch (Throwable error) { fail(error); }
                    });
                })));
        } catch (Throwable error) { fail(error); }
    }

    private void systemBinding(ServiceConnection controlConnection, IBinder control, IBinder original) throws Exception {
        java.lang.reflect.Method method = Class.forName("megler.MicroG")
            .getMethod("bindAsSystemNow", Intent.class, long.class);
        IBinder system = (IBinder) method.invoke(null, intent("mock:a"), 1000L);
        check(system == original, "system rebind retains binder");
        check(method.invoke(null, intent("mock:a"), 1000L) == system, "system binding counted once");
        state(control, "mock:a", 1, 2, 2);
        bind("mock:a", (client, shared) -> {
            check(shared == system, "system and app share binding");
            unbindService(client);
            new Handler().post(() -> {
                try {
                    state(control, "mock:a", 1, 2, 2);
                    unbindService(controlConnection);
                    done = true;
                    Log.i("GmsBrokerTest", "PASS local service, callback and lifecycle");
                    text.setText("PASS");
                    new Handler().postDelayed(() -> text.setText("PASS complete"), 1000);
                } catch (Throwable error) { fail(error); }
            });
        });
    }
}
