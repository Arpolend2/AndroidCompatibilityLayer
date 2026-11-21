package megler.test.sharedmicrogprobe;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.util.Log;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class Main extends Activity {
    private static final String TAG = "SharedMicrogProbe";
    private static final String GMS = "com.google.android.gms";
    private static final String ADS = "com.google.android.gms.ads.identifier.service.START";
    private static final String ADS_INTERFACE = "com.google.android.gms.ads.identifier.internal.IAdvertisingIdService";
    private static final String BROKER = "com.google.android.gms.common.service.START";
    private static final String BROKER_INTERFACE = "com.google.android.gms.common.internal.IGmsServiceBroker";
    private final Connection ads = new Connection();
    private final Connection broker = new Connection();
    private TextView text;
    private boolean adsBound;
    private boolean brokerBound;
    private volatile boolean destroyed;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this);
        text.setTextSize(18);
        int padding = (int) (16 * getResources().getDisplayMetrics().density);
        text.setPadding(padding, padding, padding, padding);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(text);
        setContentView(scroll);
        report("Проверка microG. Результат общего процесса подтверждается журналом.");
        try {
            int uid = Process.myUid();
            int packageUid = getPackageManager().getPackageUid(getPackageName(), 0);
            String[] packages = getPackageManager().getPackagesForUid(uid);
            if (getApplicationInfo().uid != uid || packageUid != uid ||
                    packages == null || !Arrays.asList(packages).contains(getPackageName())) {
                throw new IllegalStateException("UID приложения и PackageManager не совпадают");
            }
            report("UID приложения и PackageManager совпадают: " + uid);
            adsBound = bindService(intent(ADS), ads, Context.BIND_AUTO_CREATE);
            if (!adsBound) throw new IllegalStateException("Advertising ID: bindService отклонён");
            brokerBound = bindService(intent(BROKER), broker, Context.BIND_AUTO_CREATE);
            if (!brokerBound) throw new IllegalStateException("Broker: bindService отклонён");
            report("Ожидание ответа сервисов...");
            new Thread(this::checkServices, "microg-probe").start();
        } catch (Exception failure) {
            fail(failure);
        }
    }

    private Intent intent(String action) {
        return new Intent(action).setPackage(GMS);
    }

    private final class Connection implements ServiceConnection {
        private final CountDownLatch ready = new CountDownLatch(1);
        private volatile IBinder binder;
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            binder = service;
            ready.countDown();
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            report("Сервис отключился: " + name.getClassName());
        }
        IBinder await(String name) throws Exception {
            if (!ready.await(30, TimeUnit.SECONDS) || binder == null) {
                throw new IllegalStateException(name + ": нет подключения");
            }
            return binder;
        }
    }

    private void checkServices() {
        try {
            checkAdvertising(ads.await("Advertising ID"));
            report("Advertising ID: сервис ответил (идентификатор не показывается).");
            checkBroker(broker.await("Broker"));
            report("Broker: запрос для своего пакета принят.");
            report("ГОТОВО: оба сервиса ответили. Для проверки общего worker нужен журнал.");
        } catch (Exception failure) {
            fail(failure);
        }
    }

    private static void checkAdvertising(IBinder service) throws Exception {
        if (!ADS_INTERFACE.equals(service.getInterfaceDescriptor())) {
            throw new IllegalStateException("Advertising ID: неверный интерфейс");
        }
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInterfaceToken(ADS_INTERFACE);
            if (!service.transact(1, request, reply, 0)) {
                throw new IllegalStateException("Advertising ID: транзакция отклонена");
            }
            reply.readException();
            if (reply.readString() == null) {
                throw new IllegalStateException("Advertising ID: пустой ответ");
            }
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    private void checkBroker(IBinder service) throws Exception {
        if (!BROKER_INTERFACE.equals(service.getInterfaceDescriptor())) {
            throw new IllegalStateException("Broker: неверный интерфейс");
        }
        CountDownLatch called = new CountDownLatch(1);
        final boolean[] success = {false};
        Binder callback = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel response, int flags)
                    throws RemoteException {
                if (code != 1) return false;
                data.enforceInterface("com.google.android.gms.common.internal.IGmsCallbacks");
                success[0] = data.readInt() == 0 && data.readStrongBinder() != null;
                response.writeNoException();
                called.countDown();
                return true;
            }
        };
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInterfaceToken(BROKER_INTERFACE);
            request.writeStrongBinder(callback);
            request.writeInt(1);
            request.writeInt(0xffff4f45);
            int sizeAt = request.dataPosition(); request.writeInt(0);
            int start = request.dataPosition();
            request.writeInt(0x40001); request.writeInt(6);
            request.writeInt(0x40002); request.writeInt(39);
            request.writeInt(0x40003); request.writeInt(252432034);
            request.writeInt(0xffff0004);
            int fieldAt = request.dataPosition(); request.writeInt(0);
            int fieldStart = request.dataPosition();
            request.writeString(getPackageName());
            int end = request.dataPosition();
            request.setDataPosition(fieldAt); request.writeInt(end - fieldStart); request.setDataPosition(end);
            request.setDataPosition(sizeAt); request.writeInt(end - start); request.setDataPosition(end);
            if (!service.transact(46, request, reply, 0)) {
                throw new IllegalStateException("Broker: транзакция отклонена");
            }
            reply.readException();
            if (!called.await(10, TimeUnit.SECONDS) || !success[0]) {
                throw new IllegalStateException("Broker: нет успешного ответа");
            }
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    private void report(String message) {
        Log.i(TAG, message);
        runOnUiThread(() -> {
            if (!destroyed) {
                if (text.length() > 0) text.append("\n\n");
                text.append(message);
            }
        });
    }

    private void fail(Exception failure) {
        Log.e(TAG, "FAIL", failure);
        report("ОШИБКА: " + failure.getClass().getSimpleName() + ": " + failure.getMessage());
    }

    @Override protected void onDestroy() {
        destroyed = true;
        if (adsBound) unbindService(ads);
        if (brokerBound) unbindService(broker);
        super.onDestroy();
    }
}
