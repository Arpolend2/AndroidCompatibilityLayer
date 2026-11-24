package megler;

import android.accounts.Account;
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.accounts.IAccountAuthenticatorResponse;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/** Restricted per-channel account operations; authenticator binders never leave the worker. */
public final class SharedMicrogAccounts {
    public static final String GOOGLE = "com.google";
    private static final String DESCRIPTOR = "megler.sharedmicrog.accounts";
    static final int OP_LIST = 1, OP_ADD = 2, OP_TOKEN = 3, OP_CANCEL = 4, OP_FEATURES = 5;
    private static final int MAX_REQUESTS = 32, MAX_ACCOUNTS = 128, MAX_TEXT = 4096;
    private static final long TIMEOUT_SECONDS = 600;
    private static final ScheduledExecutorService TIMEOUTS = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread thread = new Thread(r, "megler-microg-account-timeout");
        thread.setDaemon(true);
        return thread;
    });
    private static Channel current;
    private static final ArrayList<Runnable> channelListeners = new ArrayList<>();

    private SharedMicrogAccounts() {}

    static Channel install(IBinder root) {
        Channel installed;
        synchronized (SharedMicrogAccounts.class) {
            if (root == null || current != null) throw new IllegalStateException("Shared account channel already installed");
            current = installed = new Channel(root);
        }
        notifyChannelListeners();
        return installed;
    }

    public static synchronized void addChannelListener(Runnable listener) {
        if (listener == null) throw new IllegalArgumentException("Channel listener is required");
        if (!channelListeners.contains(listener)) channelListeners.add(listener);
    }

    public static synchronized void removeChannelListener(Runnable listener) {
        channelListeners.remove(listener);
    }

    public static synchronized boolean hasChannel() { return current != null; }

    private static void notifyChannelListeners() {
        ArrayList<Runnable> listeners;
        synchronized (SharedMicrogAccounts.class) { listeners = new ArrayList<>(channelListeners); }
        for (Runnable listener : listeners) {
            try { listener.run(); }
            catch (RuntimeException e) { System.err.println("Shared microG account channel listener failed"); }
        }
    }

    static void clear(Channel expected) {
        synchronized (SharedMicrogAccounts.class) {
            if (current != expected) return;
            current = null;
        }
        expected.close();
        notifyChannelListeners();
    }

    public static Client currentClient() throws RemoteException {
        Channel channel;
        synchronized (SharedMicrogAccounts.class) { channel = current; }
        if (channel == null) throw new RemoteException("Shared account channel unavailable");
        return channel.client();
    }

    static final class Channel implements AutoCloseable {
        private final IBinder root;
        private Client client;
        private boolean closed;

        Channel(IBinder root) { this.root = root; }

        synchronized Client client() throws RemoteException {
            if (closed) throw new RemoteException("Shared account channel closed");
            if (client != null) return client;
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                if (!root.transact(SharedMicrogServiceRouter.ACCOUNTS, data, reply, 0))
                    throw new RemoteException("Shared account service unavailable");
                reply.readException();
                IBinder accountService = reply.readStrongBinder();
                if (accountService == null) throw new RemoteException("Shared account service unavailable");
                client = new Client(accountService);
                return client;
            } finally { data.recycle(); reply.recycle(); }
        }

        @Override public synchronized void close() {
            if (closed) return;
            closed = true;
            if (client != null) client.close();
            client = null;
        }
    }

    public static final class Client implements AutoCloseable {
        private final IBinder remote;
        private final AtomicInteger nextId = new AtomicInteger(1);
        private final HashMap<Integer, IAccountAuthenticatorResponse> pending = new HashMap<>();
        private final ArrayList<Runnable> closeListeners = new ArrayList<>();
        private final IBinder.DeathRecipient death = this::close;
        private boolean closed;

        public Client(IBinder remote) throws RemoteException {
            if (remote == null) throw new IllegalArgumentException("Account service is required");
            this.remote = remote;
            remote.linkToDeath(death, 0);
        }

        public Account[] accounts() throws RemoteException {
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                transact(OP_LIST, data, reply);
                int count = reply.readInt();
                if (count < 0 || count > MAX_ACCOUNTS) throw new RemoteException("Invalid account list");
                Account[] result = new Account[count];
                for (int i = 0; i < count; i++) {
                    String name = reply.readString();
                    if (!validText(name)) throw new RemoteException("Invalid account list");
                    result[i] = new Account(name, GOOGLE);
                }

                return result;
            } finally { data.recycle(); reply.recycle(); }
        }

        public boolean addCloseListener(Runnable listener) {
            if (listener == null) throw new IllegalArgumentException("Close listener is required");
            synchronized (pending) {
                if (closed) return false;
                if (!closeListeners.contains(listener)) closeListeners.add(listener);
                return true;
            }
        }

        public void removeCloseListener(Runnable listener) {
            synchronized (pending) { closeListeners.remove(listener); }
        }

        public int add(String tokenType, String[] features, Bundle options, IAccountAuthenticatorResponse callback)
                throws RemoteException {
            return start(OP_ADD, null, tokenType, features, options, callback);
        }

        public int token(Account account, String tokenType, Bundle options, IAccountAuthenticatorResponse callback)
                throws RemoteException {
            if (account == null || !GOOGLE.equals(account.type)) throw new IllegalArgumentException("Google account required");
            return start(OP_TOKEN, account.name, tokenType, null, options, callback);
        }

        public int hasFeatures(Account account, String[] features, Bundle options, IAccountAuthenticatorResponse callback)
                throws RemoteException {
            if (account == null || !GOOGLE.equals(account.type) || features == null || !validFeatures(features))
                throw new IllegalArgumentException("Google account and valid features required");
            return start(OP_FEATURES, account.name, null, features, options, callback);
        }

        private int start(int operation, String name, String tokenType, String[] features, Bundle options,
                          IAccountAuthenticatorResponse callback) throws RemoteException {
            if (callback == null) throw new IllegalArgumentException("Account callback is required");
            final int id;
            synchronized (pending) {
                if (closed || pending.size() >= MAX_REQUESTS) throw new RemoteException("Account channel unavailable");
                int candidate;
                do {
                    candidate = nextId.getAndUpdate(value -> value == Integer.MAX_VALUE ? 1 : value + 1);
                } while (pending.containsKey(candidate));
                id = candidate;
                pending.put(id, callback);
            }
            IAccountAuthenticatorResponse response = new IAccountAuthenticatorResponse.Stub() {
                @Override public void onResult(Bundle result) { deliver(id, result, 0, null); }
                @Override public void onRequestContinued() { }
                @Override public void onError(int code, String message) { deliver(id, null, code, message); }
            };
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeInt(id);
                if (operation != OP_ADD) data.writeString(name);
                data.writeString(tokenType);
                if (operation == OP_ADD || operation == OP_FEATURES) data.writeStringArray(features);
                data.writeTypedObject(options, 0);
                data.writeStrongBinder(response.asBinder());
                transact(operation, data, reply);
                return id;
            } catch (RemoteException | RuntimeException e) {
                synchronized (pending) { pending.remove(id); }
                throw e;
            } finally { data.recycle(); reply.recycle(); }
        }

        public void cancel(int id) throws RemoteException {
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeInt(id);
                transact(OP_CANCEL, data, reply);
            } finally { data.recycle(); reply.recycle(); }
        }

        private void transact(int code, Parcel data, Parcel reply) throws RemoteException {
            synchronized (pending) {
                if (closed) throw new RemoteException("Account channel closed");
            }
            if (!remote.transact(code, data, reply, 0))
                throw new RemoteException("Account operation rejected");
            reply.readException();
        }

        private void deliver(int id, Bundle result, int code, String message) {
            IAccountAuthenticatorResponse callback;
            synchronized (pending) { callback = pending.remove(id); }
            if (callback == null) return;
            try {
                if (result != null) callback.onResult(result);
                else callback.onError(code, message);
            } catch (RemoteException e) {
                System.err.println("Shared microG client account callback failed");
            }
        }

        @Override public void close() {
            ArrayList<IAccountAuthenticatorResponse> callbacks;
            ArrayList<Runnable> listeners;
            synchronized (pending) {
                if (closed) return;
                closed = true;
                callbacks = new ArrayList<>(pending.values());
                pending.clear();
                listeners = new ArrayList<>(closeListeners);
                closeListeners.clear();
            }
            remote.unlinkToDeath(death, 0);
            for (Runnable listener : listeners) {
                try { listener.run(); }
                catch (RuntimeException e) { System.err.println("Shared microG account close listener failed"); }
            }
            for (IAccountAuthenticatorResponse callback : callbacks) {
                try { callback.onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Account channel closed"); }
                catch (RemoteException e) { System.err.println("Shared microG client account callback failed"); }
            }
        }
    }

    interface Engine {
        Account[] accounts();
        AutoCloseable start(int operation, Account account, String tokenType, String[] features,
                            Bundle options, AccountAuthenticatorResponse response) throws Exception;
    }

    interface Foreground {
        boolean launch(long channelId, Intent intent);
        void dismiss(long channelId);
    }

    static final class Server implements AutoCloseable {
        private enum Outcome { VALIDATED, ERROR, CANCELED, TIMEOUT, CHANNEL_CLOSED }
        private final Engine engine;
        private final Foreground foreground;
        private final long channelId;
        private final String packageName;
        private final int uid, pid;
        private final HashMap<Integer, Request> pending = new HashMap<>();
        private boolean closed;

        Server(Engine engine, Foreground foreground, long channelId, String packageName, int uid, int pid) {
            if (engine == null || foreground == null || channelId <= 0 ||
                    !validText(packageName) || uid < 0 || pid <= 0)
                throw new IllegalArgumentException("Invalid shared account channel");
            this.engine = engine;
            this.foreground = foreground;
            this.channelId = channelId;
            this.packageName = packageName;
            this.uid = uid;
            this.pid = pid;
        }

        private final Binder binder = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException {
                if (code != OP_LIST && code != OP_ADD && code != OP_TOKEN &&
                        code != OP_CANCEL && code != OP_FEATURES) return false;
                data.enforceInterface(DESCRIPTOR);
                if (flags != 0 || reply == null) throw new RemoteException("Synchronous account request required");
                if (code == OP_LIST) {
                    synchronized (pending) {
                        if (closed) throw new RemoteException("Account channel closed");
                    }
                    if (data.dataAvail() != 0) throw new RemoteException("Invalid account list request");
                    Account[] accounts = engine.accounts();
                    if (accounts == null || accounts.length > MAX_ACCOUNTS)
                        throw new RemoteException("Invalid account list");
                    for (Account account : accounts) {
                        if (account == null || !GOOGLE.equals(account.type) || !validText(account.name))
                            throw new RemoteException("Invalid account list");
                    }
                    reply.writeNoException();
                    reply.writeInt(accounts.length);
                    for (Account account : accounts) {
                        reply.writeString(account.name);
                    }
                    return true;
                }
                int id = data.readInt();
                if (id <= 0) throw new RemoteException("Invalid account request");
                if (code == OP_CANCEL) {
                    if (data.dataAvail() != 0) throw new RemoteException("Invalid account cancellation");
                    Request request;
                    synchronized (pending) { request = pending.get(id); }
                    if (request != null) request.fail(AccountManager.ERROR_CODE_CANCELED, "Account request canceled");
                    reply.writeNoException();
                    return true;
                }
                String name = code == OP_TOKEN || code == OP_FEATURES ? data.readString() : null;
                String tokenType = data.readString();
                String[] features = code == OP_ADD || code == OP_FEATURES ? data.createStringArray() : null;
                Bundle options = data.readTypedObject(Bundle.CREATOR);
                IAccountAuthenticatorResponse response = IAccountAuthenticatorResponse.Stub.asInterface(data.readStrongBinder());
                if (data.dataAvail() != 0) throw new RemoteException("Invalid account request");
                if (response == null || ((code == OP_TOKEN || code == OP_FEATURES) && !validText(name)) ||
                        (code == OP_TOKEN && !validText(tokenType)) || (tokenType != null && !validText(tokenType)) ||
                        !validFeatures(features) || (code == OP_FEATURES && features == null))
                    throw new RemoteException("Invalid account request");
                if ((code == OP_TOKEN || code == OP_FEATURES) &&
                        !containsAccount(engine.accounts(), name))
                    throw new RemoteException("Google account is unavailable");
                Bundle safeOptions = copyOptions(options);
                safeOptions.putString(AccountManager.KEY_ANDROID_PACKAGE_NAME, packageName);
                safeOptions.putInt(AccountManager.KEY_CALLER_UID, uid);
                safeOptions.putInt(AccountManager.KEY_CALLER_PID, pid);
                Request request = new Request(id, code, name, response);
                synchronized (pending) {
                    if (closed || pending.size() >= MAX_REQUESTS || pending.containsKey(id))
                        throw new RemoteException("Account request registry unavailable");
                    pending.put(id, request);
                }
                request.schedule();
                request.start(tokenType, features, safeOptions);
                reply.writeNoException();
                return true;
            }
        };

        IBinder binder() { return binder; }

        private final class Request {
            final int id;
            final int operationCode;
            final boolean add;
            final String name;
            final IAccountAuthenticatorResponse response;
            final AccountAuthenticatorResponse authenticatorResponse;
            boolean finished;
            boolean uiLaunched;
            AutoCloseable operation;
            ScheduledFuture<?> timeout;

            Request(int id, int operation, String name, IAccountAuthenticatorResponse response) {
                this.id = id;
                this.operationCode = operation;
                this.add = operation == OP_ADD;
                this.name = name;
                this.response = response;
                this.authenticatorResponse = new AccountAuthenticatorResponse(new IAccountAuthenticatorResponse.Stub() {
                    @Override public void onResult(Bundle result) { Request.this.result(result); }
                    @Override public void onRequestContinued() { }
                    @Override public void onError(int code, String message) {
                        fail(code, "Authenticator rejected account request");
                    }
                });
            }

            void schedule() {
                synchronized (this) {
                    if (!finished) timeout = TIMEOUTS.schedule(
                        this::timedOut,
                        TIMEOUT_SECONDS, TimeUnit.SECONDS);
                }
            }

            void timedOut() {
                fail(AccountManager.ERROR_CODE_CANCELED, "Account request timed out", Outcome.TIMEOUT);
            }

            void start(String tokenType, String[] features, Bundle options) {
                Thread thread = new Thread(() -> {
                    try {
                        synchronized (this) { if (finished) return; }
                        AutoCloseable started = engine.start(operationCode, add ? null : new Account(name, GOOGLE),
                            tokenType, features, options, authenticatorResponse);
                        synchronized (this) {
                            if (!finished) { operation = started; return; }
                        }
                        closeOperation(started);
                    } catch (Exception e) {
                        fail(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator unavailable");
                    }
                }, "megler-microg-account-request");
                thread.setDaemon(true);
                thread.start();
            }

            void result(Bundle value) {
                if (value == null) { fail(AccountManager.ERROR_CODE_INVALID_RESPONSE, "Invalid authenticator response"); return; }
                Object screen = value.get(AccountManager.KEY_INTENT);
                if (screen != null && !(screen instanceof Intent)) {
                    fail(AccountManager.ERROR_CODE_INVALID_RESPONSE, "Invalid authenticator response");
                    return;
                }
                Intent intent = (Intent) screen;
                if (intent != null) {
                    synchronized (this) {
                        if (finished) return;
                        try {
                            if (foreground.launch(channelId, intent)) {
                                uiLaunched = true;
                                return;
                            }
                        }
                        catch (RuntimeException e) {
                            System.err.println("Shared microG account screen launch failed");
                        }
                    }
                    fail(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Trusted account screen unavailable");
                    return;
                }
                if (value.containsKey(AccountManager.KEY_ERROR_CODE)) {
                    fail(value.getInt(AccountManager.KEY_ERROR_CODE), "Authenticator rejected account request");
                    return;
                }
                String resultName = value.getString(AccountManager.KEY_ACCOUNT_NAME);
                String resultType = value.getString(AccountManager.KEY_ACCOUNT_TYPE);
                String token = value.getString(AccountManager.KEY_AUTHTOKEN);
                if (operationCode == OP_FEATURES) {
                    if (!value.containsKey(AccountManager.KEY_BOOLEAN_RESULT)) {
                        fail(AccountManager.ERROR_CODE_INVALID_RESPONSE, "Invalid authenticator response");
                        return;
                    }
                    Bundle safe = new Bundle();
                    safe.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, value.getBoolean(AccountManager.KEY_BOOLEAN_RESULT));
                    complete(safe, 0, null, Outcome.VALIDATED);
                    return;
                }
                if (!validText(resultName) || !GOOGLE.equals(resultType) ||
                        !containsAccount(engine.accounts(), resultName) ||
                        (!add && (!name.equals(resultName) || token == null || token.isEmpty()))) {
                    fail(AccountManager.ERROR_CODE_INVALID_RESPONSE, "Invalid authenticator response");
                    return;
                }
                Bundle safe = new Bundle();
                safe.putString(AccountManager.KEY_ACCOUNT_NAME, resultName);
                safe.putString(AccountManager.KEY_ACCOUNT_TYPE, GOOGLE);
                if (!add) safe.putString(AccountManager.KEY_AUTHTOKEN, token);
                complete(safe, 0, null, Outcome.VALIDATED);
            }

            void fail(int code, String message) {
                fail(code, message, code == AccountManager.ERROR_CODE_CANCELED ? Outcome.CANCELED : Outcome.ERROR);
            }

            void fail(int code, String message, Outcome outcome) { complete(null, code, message, outcome); }

            void complete(Bundle value, int code, String message, Outcome outcome) {
                AutoCloseable toClose;
                boolean dismissUi;
                synchronized (this) {
                    if (finished) return;
                    finished = true;
                    if (timeout != null) timeout.cancel(false);
                    toClose = operation;
                    operation = null;
                    dismissUi = uiLaunched;
                    uiLaunched = false;
                }
                synchronized (pending) { pending.remove(id, this); }
                System.err.println("Shared microG account outcome: operation=" + operationCode + " outcome=" + outcome);
                closeOperation(toClose);
                if (dismissUi) foreground.dismiss(channelId);
                try {
                    if (value != null) response.onResult(value);
                    else response.onError(code, message);
                    System.err.println("Shared microG account callback submitted");
                } catch (RemoteException e) {
                    System.err.println("Shared microG account response delivery failed");
                }
            }
        }

        @Override public void close() {
            ArrayList<Request> requests;
            synchronized (pending) {
                if (closed) return;
                closed = true;
                requests = new ArrayList<>(pending.values());
            }
            for (Request request : requests)
                request.fail(AccountManager.ERROR_CODE_CANCELED, "Account channel closed", Outcome.CHANNEL_CLOSED);
        }
    }

    private static void closeOperation(AutoCloseable operation) {
        if (operation == null) return;
        try { operation.close(); }
        catch (Exception e) { System.err.println("Shared microG account operation cleanup failed"); }
    }

    private static boolean containsAccount(Account[] accounts, String name) {
        if (accounts == null || accounts.length > MAX_ACCOUNTS) return false;
        for (Account account : accounts)
            if (account != null && GOOGLE.equals(account.type) && name.equals(account.name)) return true;
        return false;
    }

    private static boolean validText(String value) {
        return value != null && !value.isEmpty() && value.length() <= MAX_TEXT;
    }

    private static boolean validFeatures(String[] features) {
        if (features == null) return true;
        if (features.length > 128) return false;
        for (String feature : features) if (!validText(feature)) return false;
        return true;
    }

    private static Bundle copyOptions(Bundle source) throws RemoteException {
        Bundle result = new Bundle();
        if (source == null) return result;
        if (source.size() > 128) throw new RemoteException("Too many account options");
        for (String key : source.keySet()) {
            if (!validText(key)) throw new RemoteException("Invalid account option");
            if (AccountManager.KEY_ANDROID_PACKAGE_NAME.equals(key) || AccountManager.KEY_CALLER_UID.equals(key) ||
                    AccountManager.KEY_CALLER_PID.equals(key)) continue;
            if (AccountManager.KEY_PASSWORD.equals(key) || AccountManager.KEY_AUTHTOKEN.equals(key))
                throw new RemoteException("Credential options are not accepted");
            Object value = source.get(key);
            if (value == null || value instanceof String) result.putString(key, (String) value);
            else if (value instanceof Boolean) result.putBoolean(key, (Boolean) value);
            else if (value instanceof Integer) result.putInt(key, (Integer) value);
            else if (value instanceof Long) result.putLong(key, (Long) value);
            else throw new RemoteException("Unsupported account option");
        }
        return result;
    }
}
