import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import megler.NetworkNative;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

public class NetworkConnectivityTest {
    static void check(boolean value) { if (!value) throw new AssertionError(); }
    static void expect(LinkedBlockingQueue<String> events, String wanted) throws Exception {
        String actual = events.poll(3, TimeUnit.SECONDS);
        if (!wanted.equals(actual)) throw new AssertionError("wanted " + wanted + ", got " + actual);
    }
    static void quiet(LinkedBlockingQueue<String> events) throws Exception {
        String actual = events.poll(700, TimeUnit.MILLISECONDS);
        if (actual != null) throw new AssertionError("unexpected " + actual);
    }
    public static void main(String[] args) throws Exception {
        ConnectivityManager manager = new ConnectivityManager();
        NetworkNative.snapshot = 3;
        check(manager.getActiveNetwork() != null);
        check(manager.getActiveNetworkInfo().getType() == ConnectivityManager.TYPE_WIFI);
        check(manager.getNetworkInfo(null).isConnected());
        check(!manager.isActiveNetworkMetered());
        NetworkCapabilities legacy = manager.getNetworkCapabilities(null);
        check(legacy.hasTransport(NetworkCapabilities.TRANSPORT_WIFI));
        check(legacy.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED));
        check(legacy.getLinkDownstreamBandwidthKbps() == 100000);
        check(legacy.getTransportTypes() == null && legacy.getTransportTypesInternal() == 0);
        LinkedBlockingQueue<String> legacyEvents = new LinkedBlockingQueue<>();
        ConnectivityManager.NetworkCallback legacyCallback = new ConnectivityManager.NetworkCallback() {
            public void onAvailable(Network n) { legacyEvents.add("available"); }
            public void onBlockedStatusChanged(Network n, boolean b) { if (!b) legacyEvents.add("unblocked"); }
        };
        manager.registerNetworkCallback(new NetworkRequest.Builder().build(), legacyCallback);
        expect(legacyEvents, "available"); expect(legacyEvents, "unblocked");
        manager.unregisterNetworkCallback(legacyCallback);
        NetworkNative.snapshot = 0;
        check(manager.getActiveNetwork() == null && manager.getActiveNetworkInfo() == null);
        LinkedBlockingQueue<String> events = new LinkedBlockingQueue<>();
        ConnectivityManager.NetworkCallback cb = new ConnectivityManager.NetworkCallback() {
            public void onAvailable(Network n) { events.add("available"); }
            public void onCapabilitiesChanged(Network n, NetworkCapabilities c) {
                events.add(c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ? "wifi" :
                           c.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) ? "cellular" : "unknown");
                check(!c.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED));
                check(!c.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED));
            }
            public void onLost(Network n) { events.add("lost"); }
        };
        manager.registerDefaultNetworkCallback(cb);
        quiet(events);
        NetworkNative.snapshot = 2 | 4 | 32;
        expect(events, "available"); expect(events, "wifi");
        check(manager.getActiveNetworkInfo().getType() == ConnectivityManager.TYPE_WIFI);
        NetworkCapabilities caps = manager.getNetworkCapabilities(manager.getActiveNetwork());
        check(caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET));
        check(!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED));
        NetworkNative.snapshot = 0;
        check(manager.getActiveNetwork() == null);
        quiet(events);
        NetworkNative.snapshot = 1;
        expect(events, "lost");
        NetworkNative.snapshot = 2 | 8 | 32;
        expect(events, "available"); expect(events, "cellular");
        check(manager.getActiveNetworkInfo().getType() == ConnectivityManager.TYPE_MOBILE);
        NetworkNative.snapshot = 2 | 4 | 32;
        expect(events, "lost"); expect(events, "available"); expect(events, "wifi");
        manager.unregisterNetworkCallback(cb);
        NetworkNative.snapshot = 1;
        quiet(events);

        NetworkNative.snapshot = 2 | 8 | 32;
        ConnectivityManager.NetworkCallback wifiOnly = new ConnectivityManager.NetworkCallback() {
            public void onAvailable(Network n) { events.add("matched"); }
            public void onLost(Network n) { events.add("lost"); }
        };
        NetworkRequest wifi = new NetworkRequest.Builder().clearCapabilities()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI).build();
        manager.registerNetworkCallback(wifi, wifiOnly);
        quiet(events);
        NetworkNative.snapshot = 2 | 4;
        expect(events, "matched");
        NetworkNative.snapshot = 2 | 8;
        expect(events, "lost");
        manager.unregisterNetworkCallback(wifiOnly);

        NetworkNative.snapshot = 2 | 32;
        check(manager.getActiveNetworkInfo().getType() == ConnectivityManager.TYPE_NONE);
        caps = manager.getNetworkCapabilities(manager.getActiveNetwork());
        check(caps.getTransportTypes().length == 0);
        check(!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN));
        NetworkRequest validated = new NetworkRequest.Builder().clearCapabilities()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_TRUSTED)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED).build();
        NetworkNative.snapshot = 2 | 4 | 32 | 64 | 128;
        caps = manager.getNetworkCapabilities(manager.getActiveNetwork());
        check(validated.canBeSatisfiedBy(caps));
        check(caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_TRUSTED));
        check(caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED));
        check((caps.getCapabilitiesInternal() & (1L << NetworkCapabilities.NET_CAPABILITY_VALIDATED)) != 0);
        NetworkNative.snapshot = 2 | 4 | 32 | 128;
        caps = manager.getNetworkCapabilities(manager.getActiveNetwork());
        check(!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED));
        check(!validated.canBeSatisfiedBy(caps));
        NetworkNative.snapshot = 2 | 4 | 32 | 64;
        caps = manager.getNetworkCapabilities(manager.getActiveNetwork());
        check(!validated.canBeSatisfiedBy(caps));
        check(caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED));
        check(!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_TRUSTED));
        LinkedBlockingQueue<Boolean> validationEvents = new LinkedBlockingQueue<>();
        ConnectivityManager.NetworkCallback validationCallback = new ConnectivityManager.NetworkCallback() {
            public void onCapabilitiesChanged(Network n, NetworkCapabilities c) {
                validationEvents.add(validated.canBeSatisfiedBy(c));
            }
        };
        manager.registerDefaultNetworkCallback(validationCallback);
        check(Boolean.FALSE.equals(validationEvents.poll(3, TimeUnit.SECONDS)));
        NetworkNative.snapshot = 2 | 4 | 32 | 64 | 128;
        check(Boolean.TRUE.equals(validationEvents.poll(3, TimeUnit.SECONDS)));
        NetworkNative.snapshot = 2 | 4 | 32 | 128;
        check(Boolean.FALSE.equals(validationEvents.poll(3, TimeUnit.SECONDS)));
        manager.unregisterNetworkCallback(validationCallback);
        NetworkRequest notMeteredForbidden = new NetworkRequest.Builder().clearCapabilities()
                .addForbiddenCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED).build();
        check(!notMeteredForbidden.canBeSatisfiedBy(caps));

        NetworkNative.snapshot = 0;
        ConnectivityManager.NetworkCallback timeout = new ConnectivityManager.NetworkCallback() {
            public void onUnavailable() { events.add("unavailable"); }
        };
        manager.requestNetwork(wifi, timeout, 100);
        expect(events, "unavailable");
        NetworkNative.snapshot = 2 | 4;
        quiet(events);
        System.out.println("network connectivity OK");
    }
}
