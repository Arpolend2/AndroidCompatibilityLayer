package megler;

import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import java.util.ArrayList;
import java.util.List;

public final class SharedMicrogClientRouterTest {
    private static final String PACKAGE = "com.google.android.gms";
    private static final String ACTION = "com.google.android.gms.common.service.START";

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static final class Root extends Binder {
        final IBinder service = new Binder();
        final List<Integer> unbound = new ArrayList<>();
        int binds;
        int lastId;
        String lastAction;
        String lastComponent;
        String replyClass = "megler.test.gmsbroker.Broker";
        boolean noService;
        boolean nullService;
        boolean failBind;

        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (flags != 0 || reply == null) throw new RemoteException("synchronous request required");
            if (code == 1) {
                binds++;
                lastId = data.readInt();
                lastAction = data.readString();
                lastComponent = data.readString();
                if (failBind) throw new RemoteException("fixture bind failure");
                reply.writeNoException();
                reply.writeString(noService ? null : replyClass);
                reply.writeStrongBinder(noService || nullService ? null : service);
                return true;
            }
            if (code == 2) {
                unbound.add(data.readInt());
                reply.writeNoException();
                return true;
            }
            return false;
        }
    }

    private static ServiceConnection connection(IBinder[] result, ComponentName[] name) {
        return new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName component, IBinder service) {
                name[0] = component;
                result[0] = service;
            }
            @Override public void onServiceDisconnected(ComponentName component) {}
        };
    }

    private static void normalBindAndUnbind() {
        Root root = new Root();
        SharedMicrogClientRouter router = new SharedMicrogClientRouter(root, Runnable::run);
        IBinder[] service = new IBinder[1];
        ComponentName[] name = new ComponentName[1];
        ServiceConnection connection = connection(service, name);
        Intent intent = new Intent(ACTION).setPackage(PACKAGE);

        check(SharedMicrogClientRouter.canRoute(intent), "explicit GMS action is routable");
        check(router.bind(intent, connection), "bind accepted");
        check(root.binds == 1 && root.lastId > 0, "root received BIND=1 with binding id");
        check(ACTION.equals(root.lastAction) && root.lastComponent == null, "action serialized");
        check(service[0] == root.service &&
              name[0] != null && PACKAGE.equals(name[0].getPackageName()) &&
              root.replyClass.equals(name[0].getClassName()), "service callback delivered");
        check(router.hasBinding(connection), "binding retained");
        boolean duplicateRejected = false;
        try { router.bind(intent, connection); }
        catch (IllegalArgumentException expected) { duplicateRejected = true; }
        check(duplicateRejected && root.binds == 1, "duplicate connection rejected before remote bind");

        int id = root.lastId;
        router.unbind(connection);
        check(root.unbound.size() == 1 && root.unbound.get(0) == id, "root received UNBIND=2");
        check(!router.hasBinding(connection), "binding removed");
        boolean unknownRejected = false;
        try { router.unbind(connection); }
        catch (IllegalArgumentException expected) { unknownRejected = true; }
        check(unknownRejected, "unknown local binding rejected");
        router.close();
    }

    private static void explicitComponentAndUnsupportedIntent() {
        Root root = new Root();
        SharedMicrogClientRouter router = new SharedMicrogClientRouter(root, Runnable::run);
        ServiceConnection connection = connection(new IBinder[1], new ComponentName[1]);
        Intent component = new Intent().setComponent(new ComponentName(PACKAGE, "megler.test.gmsbroker.Broker"));
        check(router.bind(component, connection), "explicit GMS component bind accepted");
        check(root.lastAction == null &&
              "megler.test.gmsbroker.Broker".equals(root.lastComponent), "component serialized");
        router.unbind(connection);

        int count = root.binds;
        Intent otherPackage = new Intent(ACTION).setPackage("example.other");
        check(!SharedMicrogClientRouter.canRoute(otherPackage) &&
              !router.bind(otherPackage, connection), "other package remains local");
        Intent mismatchedComponent = new Intent().setPackage("example.other")
            .setComponent(new ComponentName(PACKAGE, "megler.test.gmsbroker.Broker"));
        check(!SharedMicrogClientRouter.canRoute(mismatchedComponent), "package/component mismatch remains local");
        Intent categorized = new Intent(ACTION).setPackage(PACKAGE).addCategory("example.category");
        check(!SharedMicrogClientRouter.canRoute(categorized) &&
              !router.bind(categorized, connection), "unsupported intent fields are not discarded");
        check(root.binds == count, "unsupported requests never reach the root");
        router.close();
    }

    private static void failedAndNullBindings() {
        Root failedRoot = new Root();
        failedRoot.failBind = true;
        SharedMicrogClientRouter failedRouter = new SharedMicrogClientRouter(failedRoot, Runnable::run);
        ServiceConnection failedConnection = connection(new IBinder[1], new ComponentName[1]);
        boolean failed = false;
        try { failedRouter.bind(new Intent(ACTION).setPackage(PACKAGE), failedConnection); }
        catch (IllegalStateException expected) { failed = true; }
        check(failed && !failedRouter.hasBinding(failedConnection), "remote errors release local reservation");
        failedRouter.close();

        Root missingRoot = new Root();
        missingRoot.noService = true;
        SharedMicrogClientRouter missingRouter = new SharedMicrogClientRouter(missingRoot, Runnable::run);
        ServiceConnection missingConnection = connection(new IBinder[1], new ComponentName[1]);
        check(!missingRouter.bind(new Intent(ACTION).setPackage(PACKAGE), missingConnection),
              "missing service reports no bind");
        check(!missingRouter.hasBinding(missingConnection) && missingRoot.unbound.size() == 1,
              "missing service binding released remotely");
        missingRouter.close();

        Root nullRoot = new Root();
        nullRoot.nullService = true;
        SharedMicrogClientRouter nullRouter = new SharedMicrogClientRouter(nullRoot, Runnable::run);
        boolean[] nullCallback = new boolean[1];
        ServiceConnection nullConnection = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder service) {
                throw new AssertionError("null binding connected");
            }
            @Override public void onServiceDisconnected(ComponentName name) {}
            @Override public void onNullBinding(ComponentName name) { nullCallback[0] = true; }
        };
        check(nullRouter.bind(new Intent(ACTION).setPackage(PACKAGE), nullConnection),
              "null service binding accepted");
        check(nullCallback[0] && nullRouter.hasBinding(nullConnection),
              "null binding callback and lifecycle retained");
        nullRouter.unbind(nullConnection);
        nullRouter.close();
    }

    private static void closeReleasesBindings() {
        Root root = new Root();
        SharedMicrogClientRouter router = new SharedMicrogClientRouter(root, Runnable::run);
        ServiceConnection connection = connection(new IBinder[1], new ComponentName[1]);
        check(router.bind(new Intent(ACTION).setPackage(PACKAGE), connection), "close fixture bind");
        int id = root.lastId;
        router.close();
        check(root.unbound.size() == 1 && root.unbound.get(0) == id, "close releases active remote binding");
        check(!router.hasBinding(connection), "close clears local bindings");
    }

    public static void main(String[] args) {
        normalBindAndUnbind();
        explicitComponentAndUnsupportedIntent();
        failedAndNullBindings();
        closeReleasesBindings();
        System.out.println("SharedMicrogClientRouterTest: PASS bind, unbind, opt-in, errors and close");
    }
}
