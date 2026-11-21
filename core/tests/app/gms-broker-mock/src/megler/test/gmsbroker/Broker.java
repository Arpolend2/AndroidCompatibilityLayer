package megler.test.gmsbroker;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;

public class Broker extends Service {
    private static final String BROKER = "com.google.android.gms.common.internal.IGmsServiceBroker";
    private static final String CALLBACK = "com.google.android.gms.common.internal.IGmsCallbacks";
    private static final class Counts { int binds, unbinds, rebinds; }
    private final java.util.HashMap<String, Counts> counts = new java.util.HashMap<>();

    private Counts counts(String key) {
        Counts result = counts.get(key);
        if (result == null) counts.put(key, result = new Counts());
        return result;
    }

    @Override public IBinder onBind(Intent intent) {
        counts(intent.getDataString()).binds++;
        return new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                if (code == 47) {
                    Counts state = counts(data.readString());
                    reply.writeInt(state.binds);
                    reply.writeInt(state.unbinds);
                    reply.writeInt(state.rebinds);
                    return true;
                }
                if (code != 46) return super.onTransact(code, data, reply, flags);
                data.enforceInterface(BROKER);
                if (data.readInt() != 39 || data.readStrongBinder() == null) throw new IllegalArgumentException("Invalid request");
                IBinder callback = data.readStrongBinder();
                if (callback == null) throw new IllegalArgumentException("Missing callback");
                Parcel result = Parcel.obtain(), response = Parcel.obtain();
                try {
                    result.writeInterfaceToken(CALLBACK);
                    result.writeInt(0);
                    result.writeString("synthetic-result");
                    if (!callback.transact(1, result, response, 0)) throw new IllegalStateException("Callback failed");
                    response.readException();
                    reply.writeNoException();
                    return true;
                } finally {
                    result.recycle();
                    response.recycle();
                }
            }
        };
    }
    @Override public boolean onUnbind(Intent intent) {
        counts(intent.getDataString()).unbinds++;
        return "mock:a".equals(intent.getDataString());
    }
    @Override public void onRebind(Intent intent) { counts(intent.getDataString()).rebinds++; }
}
