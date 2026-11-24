// Overlay of core/framework/gen-src/android/accounts/IAccountAuthenticatorResponse.java (tools/build-framework.sh).
// The response uses Android's one-way Binder transactions when the authenticator is in another process.
package android.accounts;

import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;

public interface IAccountAuthenticatorResponse extends android.os.IInterface {
    void onResult(Bundle value) throws RemoteException;
    void onRequestContinued() throws RemoteException;
    void onError(int errorCode, String errorMessage) throws RemoteException;

    abstract class Stub extends android.os.Binder implements IAccountAuthenticatorResponse {
        public static final String DESCRIPTOR = "android.accounts.IAccountAuthenticatorResponse";
        private static final int RESULT = IBinder.FIRST_CALL_TRANSACTION;
        private static final int CONTINUED = IBinder.FIRST_CALL_TRANSACTION + 1;
        private static final int ERROR = IBinder.FIRST_CALL_TRANSACTION + 2;
        public Stub() { attachInterface(this, DESCRIPTOR); }
        public static IAccountAuthenticatorResponse asInterface(IBinder b) {
            if (b == null) return null;
            android.os.IInterface local = b.queryLocalInterface(DESCRIPTOR);
            return local instanceof IAccountAuthenticatorResponse ? (IAccountAuthenticatorResponse) local : new Proxy(b);
        }
        @Override public IBinder asBinder() { return this; }

        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != RESULT && code != CONTINUED && code != ERROR)
                return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            switch (code) {
                case RESULT: onResult(data.readTypedObject(Bundle.CREATOR)); break;
                case CONTINUED: onRequestContinued(); break;
                case ERROR: onError(data.readInt(), data.readString()); break;
            }
            return true;
        }

        private static final class Proxy implements IAccountAuthenticatorResponse {
            private final IBinder remote;
            Proxy(IBinder remote) { this.remote = remote; }
            @Override public IBinder asBinder() { return remote; }
            private void send(int code, Parcel data) throws RemoteException {
                if (!remote.transact(code, data, null, IBinder.FLAG_ONEWAY))
                    throw new RemoteException("Account authenticator response transaction rejected");
            }
            @Override public void onResult(Bundle value) throws RemoteException {
                Parcel data = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    data.writeTypedObject(value, 0);
                    send(RESULT, data);
                } finally { data.recycle(); }
            }
            @Override public void onRequestContinued() throws RemoteException {
                Parcel data = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    send(CONTINUED, data);
                } finally { data.recycle(); }
            }
            @Override public void onError(int errorCode, String errorMessage) throws RemoteException {
                Parcel data = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    data.writeInt(errorCode);
                    data.writeString(errorMessage);
                    send(ERROR, data);
                } finally { data.recycle(); }
            }
        }
    }
}
