package megler;

import android.accounts.AbstractAccountAuthenticator;
import android.accounts.Account;
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.accounts.NetworkErrorException;
import android.content.ComponentName;
import android.content.Context;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;

/** Uses the authenticator inside the worker without exposing its Binder to the client. */
final class SharedMicrogAccountEngine implements SharedMicrogAccounts.Engine {
    private final Context context;

    SharedMicrogAccountEngine(Context context) {
        if (context == null) throw new IllegalArgumentException("Worker context is required");
        this.context = context;
    }

    @Override public Account[] accounts() {
        return Accounts.accounts(SharedMicrogAccounts.GOOGLE);
    }

    @Override public AutoCloseable start(int operation, Account account, String tokenType, String[] features,
                                         Bundle options, AccountAuthenticatorResponse response) {
        boolean add = operation == SharedMicrogAccounts.OP_ADD;
        if (!add && !Accounts.exists(account)) {
            response.onError(AccountManager.ERROR_CODE_BAD_ARGUMENTS, "Account is unavailable");
            return null;
        }
        Accounts.Authenticator auth = Accounts.authenticator(SharedMicrogAccounts.GOOGLE);
        if (auth == null || auth.description == null ||
                !"com.google.android.gms".equals(auth.description.packageName)) {
            response.onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Google authenticator unavailable");
            return null;
        }
        Binding binding = new Binding(auth, operation, account, tokenType, features, options, response);
        binding.bind();
        return binding;
    }

    private final class Binding implements ServiceConnection, AutoCloseable {
        final Accounts.Authenticator auth;
        final int operation;
        final boolean add;
        final Account account;
        final String tokenType;
        final String[] features;
        final Bundle options;
        final AccountAuthenticatorResponse response;
        boolean closed, bound, unbound;

        Binding(Accounts.Authenticator auth, int operation, Account account, String tokenType,
                String[] features, Bundle options, AccountAuthenticatorResponse response) {
            this.auth = auth;
            this.operation = operation;
            this.add = operation == SharedMicrogAccounts.OP_ADD;
            this.account = account;
            this.tokenType = tokenType;
            this.features = features;
            this.options = options;
            this.response = response;
        }

        void bind() {
            boolean accepted;
            try {
                accepted = auth.microG ? MicroG.bindAsSystem(auth.intent, this) : Services.bind(context, auth.intent, this);
            } catch (RuntimeException e) {
                response.onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator binding failed");
                return;
            }
            if (!accepted) response.onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator binding failed");
        }

        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            synchronized (this) { bound = true; }
            if (isClosed()) { unbind(); return; }
            AbstractAccountAuthenticator authenticator = AbstractAccountAuthenticator.of(binder);
            if (authenticator == null) {
                response.onError(AccountManager.ERROR_CODE_INVALID_RESPONSE, "Invalid authenticator");
                return;
            }
            Thread call = new Thread(() -> {
                if (isClosed()) return;
                try {
                    Bundle answer;
                    if (operation == SharedMicrogAccounts.OP_ADD)
                        answer = authenticator.addAccount(response, SharedMicrogAccounts.GOOGLE, tokenType, features, options);
                    else if (operation == SharedMicrogAccounts.OP_FEATURES)
                        answer = authenticator.hasFeatures(response, account, features);
                    else
                        answer = authenticator.getAuthToken(response, account, tokenType, options);
                    if (answer != null) response.onResult(answer);
                } catch (NetworkErrorException e) {
                    response.onError(AccountManager.ERROR_CODE_NETWORK_ERROR, "Authenticator network error");
                } catch (UnsupportedOperationException e) {
                    response.onError(AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION, "Authenticator operation unsupported");
                } catch (IllegalArgumentException e) {
                    response.onError(AccountManager.ERROR_CODE_BAD_ARGUMENTS, "Authenticator rejected arguments");
                } catch (RuntimeException e) {
                    response.onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator failed");
                }
            }, "megler-microg-authenticator");
            call.setDaemon(true);
            call.start();
        }

        @Override public void onNullBinding(ComponentName name) {
            synchronized (this) { bound = true; }
            if (isClosed()) { unbind(); return; }
            response.onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator unavailable");
        }

        @Override public void onServiceDisconnected(ComponentName name) {
            response.onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator disconnected");
        }

        private synchronized boolean isClosed() { return closed; }

        private void unbind() {
            synchronized (this) {
                if (!bound || unbound) return;
                unbound = true;
            }
            try {
                if (auth.microG) MicroG.unbind(this);
                else Services.unbind(this);
            } catch (RuntimeException e) {
                System.err.println("Shared microG authenticator unbind failed");
            }
        }

        @Override public void close() {
            synchronized (this) { closed = true; }
            unbind();
        }
    }
}
