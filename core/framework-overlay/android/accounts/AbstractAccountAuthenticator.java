// Overlay of core/framework/gen-src/android/accounts/AbstractAccountAuthenticator.java (tools/build-framework.sh).
// It differs in giving an authenticator's service something to return from onBind: the generated stub's getIBinder was
// null, and no account request could reach an authenticator (microG's for Google accounts, an app's own). The account manager
// uses the authenticator's own Transport for local calls. The transport also accepts Android's one-way
// IAccountAuthenticator transactions. The defaults of the methods that are not abstract are Android's.
package android.accounts;

import android.content.Context;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;

public abstract class AbstractAccountAuthenticator {
    public static final String KEY_CUSTOM_TOKEN_EXPIRY = "android.accounts.expiry";

    private final Transport mTransport = new Transport();

    public AbstractAccountAuthenticator(Context context) { }

    /** The binder an authenticator's service hands out. */
    public final class Transport extends Binder implements IInterface {
        private static final String DESCRIPTOR = "android.accounts.IAccountAuthenticator";

        public Transport() { attachInterface(this, DESCRIPTOR); }
        public AbstractAccountAuthenticator authenticator() { return AbstractAccountAuthenticator.this; }
        @Override public IBinder asBinder() { return this; }

        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code < IBinder.FIRST_CALL_TRANSACTION || code > IBinder.FIRST_CALL_TRANSACTION + 13)
                return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            IAccountAuthenticatorResponse remote = IAccountAuthenticatorResponse.Stub.asInterface(data.readStrongBinder());
            if (remote == null) throw new RemoteException("Authenticator response is required");
            AccountAuthenticatorResponse response = new AccountAuthenticatorResponse(remote);
            Bundle result;
            try {
                switch (code - IBinder.FIRST_CALL_TRANSACTION) {
                    case 0:
                        result = addAccount(response, data.readString(), data.readString(),
                            data.createStringArray(), data.readTypedObject(Bundle.CREATOR));
                        break;
                    case 1:
                        result = confirmCredentials(response, data.readTypedObject(Account.CREATOR),
                            data.readTypedObject(Bundle.CREATOR));
                        break;
                    case 2:
                        result = getAuthToken(response, data.readTypedObject(Account.CREATOR), data.readString(),
                            data.readTypedObject(Bundle.CREATOR));
                        break;
                    case 3:
                        result = new Bundle();
                        result.putString(AccountManager.KEY_AUTH_TOKEN_LABEL, getAuthTokenLabel(data.readString()));
                        break;
                    case 4:
                        result = updateCredentials(response, data.readTypedObject(Account.CREATOR), data.readString(),
                            data.readTypedObject(Bundle.CREATOR));
                        break;
                    case 5:
                        result = editProperties(response, data.readString());
                        break;
                    case 6:
                        result = hasFeatures(response, data.readTypedObject(Account.CREATOR), data.createStringArray());
                        break;
                    case 7:
                        result = getAccountRemovalAllowed(response, data.readTypedObject(Account.CREATOR));
                        break;
                    case 8:
                        result = getAccountCredentialsForCloning(response, data.readTypedObject(Account.CREATOR));
                        break;
                    case 9:
                        result = addAccountFromCredentials(response, data.readTypedObject(Account.CREATOR),
                            data.readTypedObject(Bundle.CREATOR));
                        break;
                    case 10:
                        result = startAddAccountSession(response, data.readString(), data.readString(),
                            data.createStringArray(), data.readTypedObject(Bundle.CREATOR));
                        break;
                    case 11:
                        result = startUpdateCredentialsSession(response, data.readTypedObject(Account.CREATOR),
                            data.readString(), data.readTypedObject(Bundle.CREATOR));
                        break;
                    case 12:
                        result = finishSession(response, data.readString(), data.readTypedObject(Bundle.CREATOR));
                        break;
                    case 13:
                        result = isCredentialsUpdateSuggested(response, data.readTypedObject(Account.CREATOR),
                            data.readString());
                        break;
                    default:
                        throw new AssertionError();
                }
            } catch (NetworkErrorException e) {
                remote.onError(AccountManager.ERROR_CODE_NETWORK_ERROR, "Authenticator network error");
                return true;
            } catch (UnsupportedOperationException e) {
                remote.onError(AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION, "Authenticator operation unsupported");
                return true;
            } catch (IllegalArgumentException e) {
                remote.onError(AccountManager.ERROR_CODE_BAD_ARGUMENTS, "Authenticator rejected arguments");
                return true;
            } catch (RuntimeException e) {
                remote.onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator failed");
                return true;
            }
            if (result != null) response.onResult(result);
            return true;
        }
    }

    public final IBinder getIBinder() { return mTransport; }

    /** The authenticator behind a binder an authenticator's service returned, or null when it is not one. */
    public static AbstractAccountAuthenticator of(IBinder binder) {
        return binder instanceof Transport ? ((Transport) binder).authenticator() : null;
    }

    public abstract Bundle editProperties(AccountAuthenticatorResponse response, String accountType);
    public abstract Bundle addAccount(AccountAuthenticatorResponse response, String accountType, String authTokenType, String[] requiredFeatures,
            Bundle options) throws NetworkErrorException;
    public abstract Bundle confirmCredentials(AccountAuthenticatorResponse response, Account account, Bundle options) throws NetworkErrorException;
    public abstract Bundle getAuthToken(AccountAuthenticatorResponse response, Account account, String authTokenType, Bundle options)
            throws NetworkErrorException;
    public abstract String getAuthTokenLabel(String authTokenType);
    public abstract Bundle updateCredentials(AccountAuthenticatorResponse response, Account account, String authTokenType, Bundle options)
            throws NetworkErrorException;
    public abstract Bundle hasFeatures(AccountAuthenticatorResponse response, Account account, String[] features) throws NetworkErrorException;

    public Bundle getAccountRemovalAllowed(AccountAuthenticatorResponse response, Account account) throws NetworkErrorException {
        final Bundle result = new Bundle();
        result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, true);
        return result;
    }
    public Bundle getAccountCredentialsForCloning(final AccountAuthenticatorResponse response, final Account account) throws NetworkErrorException {
        final Bundle result = new Bundle();
        result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, false);
        return result;
    }
    public Bundle addAccountFromCredentials(final AccountAuthenticatorResponse response, Account account, Bundle accountCredentials)
            throws NetworkErrorException {
        final Bundle result = new Bundle();
        result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, false);
        return result;
    }
    public Bundle startAddAccountSession(AccountAuthenticatorResponse response, String accountType, String authTokenType, String[] requiredFeatures,
            Bundle options) throws NetworkErrorException {
        Bundle result = new Bundle();
        result.putInt(AccountManager.KEY_ERROR_CODE, AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION);
        return result;
    }
    public Bundle startUpdateCredentialsSession(AccountAuthenticatorResponse response, Account account, String authTokenType, Bundle options)
            throws NetworkErrorException {
        Bundle result = new Bundle();
        result.putInt(AccountManager.KEY_ERROR_CODE, AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION);
        return result;
    }
    public Bundle finishSession(AccountAuthenticatorResponse response, String accountType, Bundle sessionBundle) throws NetworkErrorException {
        Bundle result = new Bundle();
        result.putInt(AccountManager.KEY_ERROR_CODE, AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION);
        return result;
    }
    public Bundle isCredentialsUpdateSuggested(final AccountAuthenticatorResponse response, Account account, String statusToken)
            throws NetworkErrorException {
        Bundle result = new Bundle();
        result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, false);
        return result;
    }
}
