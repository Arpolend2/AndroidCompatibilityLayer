# microG services

For the current DroidGuard investigation and integration status, see
[DroidGuard and Google service compatibility progress](DROIDGUARD-PROGRESS.md).

Megler can use a separately installed `com.google.android.gms` microG APK as an
optional service provider. Install the official microG Services APK in Megler
as its own application. After installation, Megler copies its APK once into the
shared Publisher Cache and records the installed Windows package identity and
cache token. Client APPX packages do not contain GmsCore; at launch, the runtime
mounts the registered APK from the shared cache. Reinstalling or updating
microG publishes a new cache entry, so already running clients can finish with
the version they loaded. Uninstalling microG clears its registration, and
clients stop using it after restart. If automatic publication fails, launching
the standalone microG app retries registration. Compatibility work with Google's proprietary
GmsCore is documented below and remains incomplete.

GmsCore services currently execute inside each client process, with a separate
Java class namespace and ELF library scope. The APK is installed and stored once,
but Megler does not yet run a centralized microG daemon or provide Android
cross-process Binder. Each client has its own service instances, but microG's
state is one for all apps (see "Shared state"). Push delivery while clients are
stopped and live cross-client provider state remain unfinished. A successful Google Play Integrity
verdict is not established. Opt-in compatibility profiles, synthetic error
callbacks and external-keybox attestation are described in the progress report.

## Experimental headless runtime

The core exposes `megler_run_headless` to start a Java APK's Application and
main looper without a launcher Activity. It remains alive without an Activity
until `megler_stop`; an Activity opened later does not end it when closed. The
normal launcher and APK report are unchanged. Headless startup requires a base
APK with `classes.dex`, Java code enabled and a compatible native ABI; an APK
that is itself a split or lacks its required ABI split is rejected.

An isolated two-guest test runs a no-Activity test Application as a Binder
server, hosts real microG services there, and exercises binding, callbacks and
shutdown. A separate isolated test starts and stops the real microG APK itself
without a launcher Activity. An additional test adds test-only code to a copy of
that APK and binds its own Advertising ID service across processes. The common
service broker also accepts the test client and invokes its callback when the
test fixture binds that client's UID to the connection; it rejects a request
naming another package. The fixture supplies this identity, not Aurora.
It uses the test client's existing hash-derived UID and a synthetic PID;
the new UID allocator is not involved in that run.
Another isolated run exercises the manager-owned `OpenChannel`: the manager
identifies the D-Bus sender, passes the server socket and its UID/PID to a
trusted test backend, and sends the other socket to the client. A separate
headless process running a modified test copy of the real microG APK receives
that identity; its Binder broker accepts the client and rejects another package.
The test substitutes Aurora's identity service and pre-seeds the UID registry
with the client's legacy hash-derived UID so the existing guest package manager
can resolve it. It does not test a production identity handoff or an unmodified
APK.
This is not a production shared microG daemon: the Aurora manager,
transport handoff and client routing do not yet start or use this runtime.
An experimental worker entry point now starts the unmodified microG APK
headlessly and accepts manager-owned channels over a private Unix
`SOCK_SEQPACKET` control socket. Each channel has its own Binder bridge and
service router; caller package, UID and PID come from the manager, not from
the client stream. Its guest package manager uses those channel-bound
identities instead of hash-derived UIDs. The worker reports bridge disconnects
and closes its channels on stop or control-socket loss. The manager must not
reuse a channel ID during the worker's lifetime, including
after a rejected or closed channel. An isolated x86_64
test exercised two clients, package-spoof rejection and cleanup against the
original APK. The production manager must still establish a trustworthy
app-to-package mapping before enabling the backend; the embedded worker mode
and Sailjail have not been verified on a device.
The experimental Binder bridge can apply a connection owner's UID and PID to
incoming calls, with thread-local restoration for nested calls and cleared
identities. It does not derive or authenticate those values from the stream.
A persistent, collision-free UID allocator is available to the manager but is
not yet connected to app launches or `PackageManager`. The manager's
`ResolveCaller` can map the actual D-Bus sender, identified by Aurora, to a
previously exported standalone and allocate its UID and a connection-scoped
virtual PID. A standalone export now records that mapping even without the
diagnostic build option. Isolated tests cover the built background manager,
unknown standalone IDs, a corrupt UID registry, client disconnect during
identification, and reconnect with the same UID and a new virtual PID. Aurora's
identity service is simulated in these tests; this has not been verified on a
device. UIDs survive manager restarts; virtual PIDs do not. This is not yet a
transport authorization decision:
the export registry is not an installation or APK-signature attestation, and
the virtual PID is not an OS PID. `OpenChannel` accepts no identity arguments
and is available only with an explicitly installed trusted backend. The backend
receives the server socket and the manager-resolved identity together; the
manager rejects new channels unless microG is selected, and revokes the socket
on D-Bus disconnect, backend release, or when microG is deselected. Manager UI
changes are applied immediately; other settings changes are checked once per
second while channels are open, so a brief external change may be missed. Invalid
or unreadable settings also revoke channels. One channel
per D-Bus connection is supported; the backend must report bridge termination
with its channel ID on the provider's thread. The production manager has no
such backend yet, and rejects channel requests. Clients must keep their D-Bus
connection alive while using the socket. Ordinary per-process mode still uses a
constant `Process.myUid()` and hash-derived UIDs that can collide. An opt-in
shared client uses its manager-assigned UID for `Process.myUid()` and its own
package; UIDs of other ordinary apps are unknown rather than hash-derived in
that mode. This does not authenticate the guest: the worker accepts caller
identities only from the manager. Production needs a trusted app-to-package
mapping and integration verified on device. The client still uses a virtual PID
from the manager for worker calls while `Process.myPid()` retains its legacy
value; when the APK is not locally mounted, existing synthetic GMS package
metadata does not establish the worker APK's version or signing identity.
An APK-supplied package name and the current UID mapping do not prove identity.

## Shared state

The core mounts `/data/megler-services` on `<storage>/services/data`, beside the
shared storage (the publisher's shared folder when storage is shared, else the
data folder). A client keeps microG's state in
`/data/megler-services/com.google.android.gms`, and microG's own package, run as
an app (its settings), has that same folder for its private files
(`core/src/run/mg_run.c`). What the user sets in microG's settings (device
registration, cloud messaging), the check-in's Android ID and microG's
databases are therefore every client's. Preferences are read when a process
first opens them: a change made while another app runs reaches it at its next
start.

## Check-in and broadcasts

`megler.MicroG` reads microG's receivers and routes a broadcast that names
microG's package or one of its receivers to them (`megler.Broadcasts.send`).
When microG's code first runs in an app (its Application is created), its
receivers that list `BOOT_COMPLETED` hear it once in that process, as after
Android's boot. microG's check-in trigger then checks in when device
registration is on and the last check-in is older than twelve hours. microG
keeps registration off until it is turned on in its settings; Megler turns it
on (`checkin_enable_service`, `MicroG.registrationOn`) unless the user has set
it there, since Play Store has no Android ID without a check-in and its front
page refused every request ("Server error", package 570). Under qemu, with
registration on, Play Store's process checked in ("Checked in as ..."),
microG stored the Android ID and filled its GServices database, and Play Store
read the Android ID from GServices on its next start. GServices
(`com.google.android.gsf.gservices`) is microG's own provider when microG is the
Google Play services (`megler.GmsProviders`, `PackageManager.resolveContentProvider`):
Google Services Framework, mounted beside it for Google's own Play services,
answered it before, and its database never had microG's Android ID.

## Accounts

`android.accounts.AccountManager` keeps accounts in
`/data/megler-services/accounts/accounts.json` (`megler.Accounts`), one file for
all apps, read again when another process wrote it. Requests that need an
account type's authenticator bind its service -- microG's `GoogleLoginService`
for `com.google`, which is not exported, as the system binds it -- and call it
off the main thread with the caller's package, uid and pid, as
AccountManagerService does. An authenticator's screen (microG's sign-in) is
started for a caller that gave an activity; when it opens in microG's own app,
`addAccount` ends when the new account appears in the shared file. A caller
that starts the screen itself for a result (Play Store calls `addAccount` with
no activity and starts the intent it gets with `startActivityForResult`) gets
the result the sign-in sets as it finishes (`RESULT_OK`): `megler.AppResults`
passes it through `/data/megler-services/results/`, and microG's app brings the
caller to the front again. There is no account chooser, no visibility, and no
session API.

A content provider's caller (`ContentProvider.getCallingPackage`) is the app:
every caller is in the app's one process, whose uid and pid are the app's, and
microG's providers hold the package they are told against the one the pid
names. It was the provider's own package, and microG's account provider, which
an app's `GoogleAuthUtil` asks for the Google accounts, refused every app:
"UID [10100] is not related to packageName [com.google.android.gms] (seems to
be com.google.android.apps.classroom)". The test app `microg` asks it
(`get_accounts`): past that check it reads the caller's certificate, which the
unsigned test app has none of ("CertData to small"); an app from a store is
signed.

## Parcels between a client and microG

Megler's `Parcel` keeps a Parcelable as the object itself. A client and microG
have different classes of the same names (the client library's
`GetServiceRequest`, `Feature`, `Scope`), so a reader that names its creator or
class loader gets an object of its own class: one of another class of that name
is written out and read in through the reader's creator, as between processes
(`core/framework-overlay/android/os/Parcel.java`). The typed readers also read
Android's own layout (the count, then each item written out in place), which the
client library's SafeParcelWriter writes itself. Without both, microG's broker
could not read Play Store's `GetServiceRequest`. Bundles are still passed as
they are; a Parcelable of the client's class inside a Bundle reaches microG as
the client's object.

## Dynamite modules

microG loads a client's dynamite module (measurement, provider installer)
through a `PathClassLoader` it makes over its own APK. The runtime makes such a
loader another name of microG's class scope (`mg_dvm_art.c`): the module's
classes are microG's, and its native libraries load from microG's APK in
microG's ELF scope. The provider installer thus puts microG's Conscrypt
(`GmsCore_OpenSSL`) first for the client, as on Android. Classes a module takes
from its client (`MERGED_CLASSES`, Cronet's API) are not followed yet; the log
says so when a module names any.

The client library asks which module to load of Google Play services'
provider (`content://com.google.android.gms.chimera`), under the class lock
of its `DynamiteModule`. A thread that asks the provider before their
Application is attached waits for the app's main thread to attach it
(`MicroG.attachOnMain`, ten seconds at most), and ends the wait when their
Application is attached by anything the main thread runs. It does not wait
for a main thread that waits for it: one that stands waiting to enter a
monitor the asking thread holds, or one whose holder waits for one of the
asking thread's (`Native.mainWaitsForMe`, the runtime's `mg_dvm_waits_for`,
asked every 50 ms of the wait), attaches nothing until the thread goes on,
and the provider then answers before their Application is attached; the log
says so once ("I/MicroG: Google Play services' provider is asked by a thread
(...) holding a monitor that the main thread waits to enter"). Citymapper's
`Application.onCreate` initializes Google Maps, which loads a module under
that class lock, while a thread of the measurement library's, the lock held,
asks the provider for its own: the app stood ten seconds at every start
(0.1.0.690). On Android the provider is their process's and answers whatever
the app's main thread does.

## Google's own Play services: their process, and their module scans

Google's own Play services (not microG) run in the app that hosts them, and
their application is told that this process is its own: the process name of
their `ApplicationInfo` is the app's (`MicroG`'s constructor, said in the log:
"Google Play services' application runs in this process, ..., as its main
one"), which is what `Application.getProcessName` gives their code here. Their
components keep the process names of their manifest.

Their code takes the process whose name is its application's for its main one
(`bjej.i` in 26.41.31: `getApplicationInfo().processName.equals(
Application.getProcessName())`), and only there does `GmsModuleFinder`
make their module configuration again at once when a scan finds it changed
(`aeac.z`, its reason "Features changed", "Device properties changed",
"Verify failed" or "Unmigrated"). Anywhere else the scan asks for another
(`startService(MODULE_SCAN)`, which `ConfigUpdateIntentOperation` takes) and
leaves it to that one. With "com.google.android.gms" there and the app's
name here, no scan was ever the main process's, and each asked for the next:
five a second, without end, in every app that hosted Google's own Play
services. The log showed it as "Beginning GMS chimera module scan (logged 1000
times)" every minute or so, the same three files opened 200 times in 8 to 9 s
(`current_fileapks.pb`, `stored_modulesets.pb`, `ChimeraConfigService.xml`),
and a collection every three seconds, 30 to 40 MB of each made by the thread
"[com.google.android.gms.chimera.container.intentoperation..." (Aliucord,
Facebook, VTB, Ozon and Play Store on 0.1.0.671). The other readers of that
comparison in 26.41.31 are the measurement's tag of its process's kind and
the feedback screens; and `PhenotypeCommittedOperation` scans again after a
change of flags in the main process only.

`MicroG.start` counts the starts of their services by what they were started
for, and one asked for a hundred times in ten seconds is said, with what asked
(three times at most each): "Google Play services asked for <action> ->
<service> 100 times in N ms". The test app `gms-host` hosts a stand-in for
their package with one service (`gms-own`, no microG in it) and starts it:
"main process true application=megler.test.gmshost current=megler.test.gmshost"
(`tools/build-tests.sh arm`).

## Google's own Play services: their network location

Google's Play services find the phone from the Wi-Fi networks around it (their
network location module, in the app's process here, started by their fused
provider). Three things it needs were missing, and are there now (docs/JAVA.md,
"Wi-Fi, and Google's network location", which says where each was found in
26.41.31): scans through `WifiManager` (`startScan`, the broadcast,
`getScanResults`; it uses them where `getSystemService("wifiscanner")` is null,
as it is here); Google Location Accuracy, `network_location_opt_in` in Google's
settings (`content://com.google.settings/partner`, Google Services Framework's
provider), which `megler.LocationAccuracy` asks of the user once an app that
holds the location binds their location API, and writes; and `PendingIntent`'s
sends with an `OnFinished` and options, by which the module hands each fix to
the fused provider, and which sent nothing. Their fixes are seen on that way
(`megler.LocationService.sent`) and said in the log. The module also runs
on Google Play services' own alarms (`GmsAlarm`: `AlarmManager`'s forms with
an `Executor` and a `WorkSource`, which did nothing up to 0.1.0.684, so
that a phone scanned once and never again), asks for the cells
at each scan (`requestCellInfoUpdate`, answered now with none), and asks
its server with an `X-Goog-Spatula` header of Google Play services' AppCert
service, signed with the device key where the phone has one (JAVA.md says
what a phone without one sends). With microG nothing of this is asked:
microG's own location backends are its own.

## Google's own Play services: DroidGuard

DroidGuard is Google Play services' check of the device it runs on (their
module `com.google.android.gms.droidguard`). For a flow -- the check-in, the
device key, a Play Integrity token, a sign-in -- it asks Google's server for
a program (`androidantiabuse/v1/x/create`), runs it in a virtual machine the
server names (an APK of 24 classes with a native library `libd<12 hex
digits>.so`, kept in `app_dg_cache/<the APK's SHA-1>/`, from
`www.gstatic.com/droidguard/`), and hands the machine's bytes to whoever
asked, who sends them to Google with its own request. Here the module runs
in the app's process with the rest of Google Play services: the machine's APK
gets a class scope of its own under theirs ("a dalvik/system/DexClassLoader
over .../app_dg_cache/.../the.apk: scope N of its own" in the log), and its
library loads in their ELF scope ("ld: libdB21738DAE92C.so at ..."), after
an `UnsatisfiedLinkError` for the library's path inside the APK, which the
machine's own Java answers by copying the library out of it, as it does
where a linker cannot load from an APK.

Since 26.4x Google Play services first asks for a "full" program, runs it,
and sends its result back asking for a "fast" one (DroidGuard's "fast
refresh": the log's tag `DGFRR`, `bvre` in 26.41.31, `byts` in 26.43.30),
which then serves every flow that may use it; without one each flow asks
for a program of its own, as before.

**What fails, and where.** On the Lumia 650 (0.1.0.685) and the Elite x3
(0.1.0.681), with Google Play services 26.43.30, every flow logs "E/DGFRR: FR
error ... bywg: byteCode" and "W/DG: [flow devicekey] DroidGuardRunner.
initSignalCollection() exception" (checkin, cryptauth_synckeys_request,
po-token-fast, constellation_verify and recaptchabase-android likewise), and
the device key is refused ("W/GLSUser: [AppCertManager] Exception while
requesting key: ... dmyc: Parsing Exception ... Protocol message end-group
tag did not match expected tag", then "[DeviceKeyStore] Cannot load key:
Device key file not found"). Under qemu, with 26.41.31's armeabi-v7a APK as
the app and the same machine (AA6305A5..., `libdB21738DAE92C.so`), it is the
same, and there it was followed through (`trace-calls-within-com.google.ccc.
abuse.droidguard`, `trace-syscalls`, `trace-files-libdB`, `trace-calls-bvts`,
`trace-calls-java.io.PipedOutputStream`):

- no class-loading or native execution failure was observed on this path:
  the machine's classes load, its library loads and runs, `initNative` and
  `ssNative` return, and it produces results of 1 to 2 KB ("DroidGuard.ss
  returns [B of 1898: 0a06..."), the "full" one going to Google in the "fast"
  request;
- Google's server answers that request with a signed answer that holds no
  program -- no `byteCode` (field 1, the first of the fields 26.41.31's
  `bvrp.a` looks for, and the one it names) and no extra bytes to carry back
  (field 9: "bvts.<init>(Ljava/lang/String;[B)V "byteCode" [B of 0");
- the device-key request (`android.googleapis.com/auth/devicekey`, with the
  result of the "devicekey" flow's own program) is answered with Google's
  HTML page "Error 400 (Bad Request)" (1664 bytes) where its protocol buffer
  was expected. Google's other answers come whole: the check-in succeeds,
  the programs' answers verify their signatures, and on the Lumia the
  account tokens' answers decrypt.

**What the machine is told here that no Android device tells it.** Through
JNI it reads the displays (`Display.getType()`, a stub here, says
`TYPE_UNKNOWN` for the phone's own screen), the memory, Google Play services'
package and signatures, and the features (`android.hardware.strongbox_keystore`,
`android.software.device_id_attestation`, `android.software.verified_boot`:
none here); and it makes two EC keys in `AndroidKeyStore` with an attestation
challenge (`unstable.<hash>.A-...` and `.B-...`), which Megler's key store
refuses ("ProviderException: Failed to generate attestation certificate
chain"), as a device whose keymaster cannot attest -- Google Play services'
check-in asks for such a key too ("KeyAttestationHandler: Failed to generate
key attestation"). By number it looks for `/sys/fs/selinux/enforce`, which
every Android device since 4.3 has, and finds nothing there. And the device
it is told of is a Pixel of 2016 (`sailfish`) on Android 14, with a
fingerprint Google never built, and a 32-bit ARM one alone.

**What was tried, under qemu**, with code and flags of the experiments'
own, not kept in Megler: the attested pairs made as Android's software
keymaster makes them where there is no trusted environment (AOSP's
`system/keymaster`: its software attestation keys and certificates, the
key's description at the security level Software, checked with openssl),
first with AOSP's EC chain, whose batch certificate expired in January 2026,
then with its RSA one, unexpired, together with a `/sys/fs/selinux/enforce`
that says 1. The machine took both: it read the file, its pairs were made,
and its "full" result grew from 1898 to 3750 bytes with their chains in it.
Google's answers were the same: no "fast" program, an empty extra, and the
device key's "Error 400". These experiments did not fix either failure.

**What this establishes, and what it does not.** The immediate failure is
the program-creation response's missing `byteCode`, after native execution
of the "full" program. In 26.41.31, `bvre.a` obtains and runs "full", puts
its result in the "fast" request, and calls `bvrp.a` again. That parser
verifies the response signature through `bvro.a` before checking the
`byteCode` presence bit; a failed signature has a different exception,
"Creation response signature verification failed." The `byteCode` error
therefore does not mean a missing native entry point or a bytecode
interpreter exception. The device-key protobuf error is a separate failure
to parse an HTML response; it does not by itself identify why that request
was rejected.

Successful native returns and the size of the results do not establish
that Megler answered every probe with Android's semantics. Neither response
explains Google's decision. The missing attestation and other environment
differences above are candidates, not an established root cause; the two
software-attestation experiments do not prove that hardware attestation is
the only remedy. No change has been demonstrated to obtain a device key or
a Google Wi-Fi location fix. Play Integrity and SafetyNet were not validated
by these runs.

**Rechecked with 26.43.30, 2026-11-11.** A fresh qemu run on the current
integration base, with versionCode 264330027 (190300-1006944126,
armeabi-v7a), reproduces the same path. The APK was obtained from
[APK Cafe](https://apk.cafe/download?file_id=3543596/google-play-services),
SHA-256 `5e4c6fc21493dfa5113bbb7b5194a972649dfdc7e3e08e772393702d2033da8c`;
Android's `apksigner verify --print-certs` verifies its signature and reports
certificate SHA-256 `7ce83c1b71f3d572fed04c8d40c5cb10ff75e6d87d9df6fbd53f0468c2905053`.
This is the same version and variant as the phone log, not an APK extracted
from the phone. Its decompiled `byts.a` has the full-to-fast sequence above,
and `byud.a` has the same signature and field-presence checks as `bvrp.a`.
The fresh run downloads the same AA6305A5... VM, loads its native library,
and returns 1960 bytes from `DroidGuard.ss`; `byuc.a` then returns 1 (the
fast response's signature verifies), followed on that thread by
`bywg.<init>(Ljava/lang/String;[B)V "byteCode" [B of 0`. Check-in succeeds.
The result is also reproduced with 26.41.31 on fresh data (1939 bytes from
the full VM, followed by `bvts` with the same missing field and empty extra).

The same 26.43.30 APK was then hosted by a separate diagnostic app under
qemu, through `MG_JAVA_GMS_APK`, with Google Services Framework beside it.
The client binds the real DroidGuard service and invokes GMS's own
`arcf` (`AppCertManager`) on a worker thread. The service connects, the full
VM returns 1921 bytes, the fast response's signature verifies, and its
missing `byteCode` throws as above. Check-in succeeds; the Android ID read
by `brtb.d(context)`, which `arcf.b` uses, is nonzero. The device-key
response is 1664 bytes starting with HTML, followed by `dmyc: Parsing
Exception` and the missing device-key file, as on the phone. This reproduces
the failure with GMS inside a client's process, not just as the main app.

One further candidate was tested in that client on separate fresh data.
Megler reports its fixed Google certificate (`f0fd6c5b...`) for GMS, whereas
this APK's certificate is `7ce83c1b...`. Before binding, the probe replaced
the hosted GMS `PackageInfo.signatures` and `signingInfo` with the APK's
actual signer, read by `ApkSigner.fromSigningBlock`; the probe logs that the
replacement was applied. The full VM then returned 1956 bytes, but the
verified fast response still lacked `byteCode` and the device-key response
was still 1664 bytes of HTML. Replacing those two fields alone did not fix
the failure. The experiment is confined to the diagnostic client; no
certificate substitution change is kept in the runtime.

**The complete full-to-fast exchange.** The opt-in diagnostic app
`core/tests/app/droidguard` captures GmsCore 264330027's request protobufs
(`byvy.a`), the exact response bodies and signatures given to its verifier
(`byuc.a`), and the full VM's result (`byun.c`). Its hooks call the original
methods and return their results unchanged. The hook bridge is loaded in a
child DEX scope, outside the parent class path the VM searches. The app
captures at most 128 calls, with at most 2 MiB per byte array, in its own
`files/capture` folder. It is built with the other Java test apps and is run
explicitly, not by the normal suites; it needs internet access and Google's
26.43.30 APK mounted through `MG_JAVA_GMS_APK`.

On fresh test data, an independent protobuf reader finds the full VM's 1882
bytes unchanged in field 10 of the fast request. The fast response is
exactly 30 bytes: field 2 is an empty URL, field 3 the 20-byte VM checksum,
field 4 is 604800 seconds, and field 6 is true. Field 1 (`byteCode`) is absent.
OpenSSL independently verifies its SHA-256/RSA signature with the public key
from `byuc`, so this is not just Megler's signature verifier accepting it.
GMS carries field 6 to `bytx.d`; after parsing, `byts.a` would throw `bytt`,
whose message is "Server requested the module to retry the full collection."
Its parser first throws for the missing bytecode, however. This retry flag
does not identify the signal the server objected to.

The full reply's program has a ten-second lifetime. In a timed run the reply
arrives at elapsed time 88545 ms and the fast request starts at 89335 ms:
790 ms later, including 428 ms running the VM. Expiry of that window does
not explain this failure. Another fresh run uses
`no-plt-rewrite-libdB21738DAE92C.so`; the linker confirms zero PLT entries
rewritten, but the same verified 30-byte response without bytecode returns.
Leaving this library's PLT unchanged alone does not fix it. The main payload
of the VM result is opaque in these captures; its bytes and the retry flag
do not by themselves establish which environment check failed.

To repeat the capture under qemu after building the test app, with the same
heavy-job lock and `MEGLER_JOBS=4` as any test run:

```sh
data=$(mktemp -d /tmp/megler-droidguard.XXXXXX)
MG_JAVA_GMS_APK=/path/to/GmsCore.apk \
MG_JAVA_GSF_APK=/path/to/GoogleServicesFramework.apk \
qemu-arm -L /usr/arm-linux-gnueabihf /path/to/mg_tests --java app \
  /path/to/droidguard.apk "$data" /path/to/android-root - \
  0 720x1280@320 wait:120000
python3 -I tools/droidguard-inspect.py \
  "$data/apps/megler.test.droidguard/data/files/capture"
```

`tools/droidguard-inspect.py` checks field 10 against the preceding VM result
on the same thread and describes the response's fields without printing
device identifiers or the token. It reports mismatches and malformed
captures as errors. A successful inspection means the capture is consistent,
not that DroidGuard obtained a fast program. Instrumentation may affect the
VM's observations; a proposed fix must also pass an uninstrumented run.

**The device-key token and an independent HTTP submission.** A separate
fresh 26.43.30 client run captures `alkl.b("devicekey", map)` and the `jofz`
message passed to `atvj.b`, without the native-memory inspection used in
other experiments. The flow downloads its own signed program, initializes
and runs it, and returns a non-null 2996-byte base64 token. The independent
protobuf reader finds that token unchanged in field 1 of the 3047-byte
request. The request also contains a nonzero Android ID, a session nonce,
`missing_token`, and SDK/GMS version metadata. A nonempty token is therefore
being sent; the fast-program failure alone does not establish that the
device-key request has no DroidGuard token.

In another fresh run, the diagnostic hook retains the device-key request
before calling the original network method and throws deliberately, so GMS
does not submit it. Its first submission is instead by WSL's `curl`, over
HTTP/1.1, with the captured protobuf and GMS headers (`app`, `gmscoreFlow`,
`gmsversion`, `device`, and `Content-Type: application/x-protobuf`). That
3022-byte request, carrying a new 2971-byte token, gets HTTP 400 and 1664
bytes of Google's HTML error page too. This is not a replay of an already
submitted token. The alternate client changes HTTP/TLS defaults and has no
successful Android control; this does not certify every detail of Megler's
transport. It does reproduce the rejection outside that transport.

Both experiments clear only the diagnostic client's `arcf` retry timestamp
to make it attempt the request during the short test. They neither replace
the token nor change the keystore, package certificate or VM's answers. No
runtime change from these experiments is retained. The server's reason for
rejecting the request remains unknown; no device-key or Wi-Fi-positioning
fix has been demonstrated.

**Signing-certificate rotation changes the interpretation of that experiment.**
The same APK's v3 signing block has proof-of-rotation attribute `0x3ba06f8c`:
its first certificate hashes to
`f0fd6c5b410f25cb25c3b53346c8972fae30f8ee7411df910480ad6b2d60db83`,
and its next to the `7ce83c1b...` signer above. This is the
[AOSP v3 rotation structure](https://source.android.com/docs/security/features/apksigning/v3).
Android's legacy [`PackageInfo.signatures`](https://developer.android.com/reference/android/content/pm/PackageInfo#signatures)
returns the oldest certificate after rotation. The native trace asks for
`GET_SIGNATURES` (64), so Megler's old `f0fd6c5b...` value matches that API's
expected certificate for this APK. Replacing it with the current signer was
an experiment, not a demonstrated correction of the legacy field. It must
not be used as evidence that the old certificate caused DroidGuard's failure.
This does not validate the separate `SigningInfo` API's rotation semantics.

**Native field reads and the hosted package's permissions.** A temporary
JNI field trace on 26.43.30 shows that the full VM asks for GmsCore's
`PackageInfo` with `GET_PERMISSIONS`, then reads `requestedPermissions` as
null. The service host parsed components but never those manifest tags;
`GET_PERMISSIONS` could also take its basic version/signature-only path.

The host now reads `uses-permission`, `uses-permission-sdk-23` and
`uses-permission-sdk-m` outside the application, including declarations after
it, deduplicates names, and respects `maxSdkVersion` as the client's manifest
reader does. `GET_PERMISSIONS` asks for the complete manifest. Its granted
bits agree with the hosted package context's existing permission policy.
The gms-host/gms-own fixture checks this on a cold query: the hosted
package's own three permissions, one duplicate, an expired declaration,
and one declaration after the application. The new check fails against the
old framework (59 checks, 1 failed) and passes with the change (59 checks,
0 failed). A fresh, uninstrumented hosted GMS run with the changed framework
still throws `bywg("byteCode", empty)` after the full VM returns 1888 bytes;
its AppCertManager still fails to obtain a key.

This corrects an observed answer to the VM, but is not a demonstrated
DroidGuard fix. In separate client experiments, supplying all 401 applicable
permissions from the exact GMS APK still gets the verified 30-byte fast
response without bytecode; supplying both those permissions and the APK's
actual signer gets the same response. Neither experiment obtains a device
key. The native field instrumentation and certificate substitution are not
retained in the runtime.

A further fresh run changes `Display.getType()` from its stub's
`TYPE_UNKNOWN` (0) to `TYPE_INTERNAL` (1), using the populated permissions
and keeping the correct legacy signing certificate. The hook logs each
replacement, but the fast response is still verified, 30 bytes long and
without bytecode, and no device key is obtained. That answer alone does not
fix either failure; the display override is confined to the diagnostic app.

**What a Spatula header proves.** In 26.43.30, `arcd.a(package)` reads
the key through `arcg.b()` (`device_key` on disk), attempts `arcf.b()` if it is missing,
and can still return a nonempty header if no key was obtained. Its base64
protobuf (`hyfk`) then carries only the app/certificate message in field 1
and a device identifier in field 3. With a device key it additionally sets field 2
to the HMAC-SHA256 from `arcf.c`, and fields 4 and 5 to the key metadata.
Thus the presence of `X-Goog-Spatula` is not a signing check: look for its
nonempty field 2. That establishes local MAC generation, not server
acceptance or a location fix. Do not copy a captured header or its device
identifier into the repository.

**Reference Android and attestation errors (2026-11-12).** On the explicitly
selected rooted Android 11 reference device, GMS 26.26.34 (262634022) reports
both signing certificates in its rotation history and the oldest certificate
through the legacy API, as above. A scoped EC key-generation probe with a public
test challenge fails with `ProviderException` caused by
`android.security.KeyStoreException`, internal error `-100` (unimplemented).
The probe removes only its own temporary alias and checks that it is absent.
No existing device key is read or removed.

That device nevertheless completes a fresh full/fast exchange. Its ordinary
Cronet request fails with `ERR_CERT_INVALID` (internal code -207); standard
Android HTTPS to the same host validates TLS. With the user's explicit approval,
a temporary Frida agent sends the original GMS-built program requests through
that verified HTTPS client, retaining Google's protobuf and RSA verification.
Neither TLS verification nor the VM result is replaced. The full response has
79,460 bytes of bytecode and a 10-second TTL. The native VM returns 24,987 bytes;
those exact bytes become the fast request's field 10. Google returns a verified
fast response with 82,668 bytes of bytecode, TTL 604800, and no retry-full request.
The VM observes the `-100` attestation failure during this successful exchange.
This rules out unsupported attestation alone as an explanation of Megler's
failure; the Android ABI and GMS release differ from the phone's ARM32 26.43.30.
Only class names, packet structure, lengths and verification results are
exported from this reference exchange, not its requests, tokens or identifiers.

Megler's attestation failure originally had no cause, and the generated
`KeyStoreException` discarded the code and message. The provider now attaches
`KeyStoreException(-100, "Not implemented")`; the exception preserves its code
and message and classifies that failure as permanent, system-wide and
`ERROR_UNIMPLEMENTED`, following the
[Android exception mapping](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/keystore/java/android/security/KeyStoreException.java).
Other codes currently use the public API's fallback classification, rather than a complete KeyMint error table. The data app's
`keystore-certificates` test fails before the change (`error=0`, 276 steps
passed, one failed) and passes afterward (`error=-100`, 277 steps passed,
445 runner checks, zero failed). A fresh uninstrumented 26.43.30 qemu run still
fails the fast-response bytecode check after the full VM returns 2057 bytes
(2064 with Android 14 error wording),
and device-key acquisition still fails. This is an Android API compatibility
fix, not a demonstrated DroidGuard fix.

**Matching the reference release and ARM32 program.** The opt-in capture app
also supports GMS 26.26.34 (262634022), using `bohf.a`, `bofj.a`, and
`bofu.c`/`bofu.b` for the same pass-through captures. Under qemu this release
also selects full refresh: the VM returns 2134 bytes, those exact bytes become
fast field 10, and Google returns the same verified 30-byte response without
bytecode. Its full/fast timings are 245 ms for the VM and 72 ms for the fast
request. Changing GMS releases therefore does not remove the failure.

A separate offline diagnostic APK, with no network permission and its own
storage, runs the exact ARM32 VM APK and full bytecode from that qemu exchange
on the reference device. The first run returns 22,957 bytes on Android and
2205 bytes in Megler. A bounded JNI-name tracer in that APK observes additional
collection on Android; the exact result length varies with state and tracing
(about 33.9 KB in later Android runs). It exports only class/method/field names,
signatures, lookup success, and result lengths. It never exports the result.
The comparison uses no GMS private storage. Matching SDK 30 and the two observed
unavailable API lookups (`getDeviceId` and `isTransientFailure`) inside the
isolated Megler diagnostic still returns only 2129 bytes. Those experimental
API overrides are not runtime fixes and are not retained.

A separate offline experiment supplies a simulated SELinux status file containing
`1`: the VM's `fstatat64` and `openat` then succeed, but it still returns just
2124 bytes. The simulated file is not retained, since it would not establish
SELinux enforcement on Windows. The native property trace finds requests for
`ro.boot.verifiedbootstate` and `ro.boot.vbmeta.digest`; both are absent in
Megler and their values are empty on the reference device. This observation
alone does not establish equal native property-presence semantics.
The smaller result
alone does not identify the failed signal or establish a server verdict for the
offline run; it supplies a reproducible native-collection comparison independent
of GMS version, VM architecture and network transport.

**Validation after the attestation-cause fix (2026-11-12).** With all temporary
native tracing and simulated answers removed, `tools/build-tests.sh arm` and
`tools/build-tests.sh win` both finish with exit code 0 and zero failed checks.
ARM's main runs report 8751 and 8910 checks; Windows reports 8210 and 8332.
The data app passes all 277 steps on both runs, including the strengthened
keystore assertion (445 and 460 runner checks). Both suites run serially under
the shared heavy-build lock with `MEGLER_JOBS=4`. Local logs are
`build/droidguard-keystore-arm.log`, `build/droidguard-keystore-win.log` and
`build/droidguard-keystore-exit.txt`. These are compatibility-suite results;
the real GMS reproductions above still fail DroidGuard and device-key issuance.

A subsequent offline qemu `-strace` run with `trace-hooks`, on the same retained
runtime, returns 2131 bytes and exits normally. It emits no guest hook-code
trace and shows no direct host-kernel opens of the three inspected paths
(`/dev/binder`, `/sys/fs/selinux/enforce`, `/proc/cpuinfo`). This limited check
does not demonstrate a generated-code or syscall-interception failure. It is
not a full audit of all kernel calls or of the VM's environment checks.

**Comparing the same signed APK on Android and Megler.** Later offline runs
use the same signed diagnostic APK on both systems. The app verifies its
73,066-byte program at runtime and both copies contain the same 293,088-byte
ARM32 native library. Android returns about 33.9 KB and Megler about 2.2 KB.
Turning off Megler's JIT or matching the reference's reported CPU capabilities
and core count leaves Megler's result near 2.2 KB. Compressing the native entry
in the VM APK forces the Android run to extract the library and still gives
about 34 KB. These controls cover the tested JIT setting, CPU metadata and
loading paths.

The JNI trace also records whether a lookup succeeded and the caller's
offset in the VM library. The trace does not attribute the first
`ClassLoader.getParent` on Android to the VM. The common `getPackageInfo` and
`generateKeyPair` lookups come from the same VM instruction, `0x3cff0`, on both
systems. Both runs make
two attested key-generation attempts and get a `ProviderException` each
time. After the second error, Android looks up `ServiceManager.getService`
from `0x3cd28`, gets a binder, calls `dump` and continues with more Java
queries. Megler proceeds to package checks without that lookup. Android
generates a non-attested key much later in the trace. The order does not
establish that this is an immediate fallback from either attestation error.

The early feature-query results agree in call order. The primary display's
type and flags differ. In the isolated APK, setting the test screen to the
reference's dimensions and supplying its type and flags still gives 2,229
bytes in Megler. With the Megler type and flags supplied to the Android run
the VM returns 33,899 bytes. The changes to those answers were confined to
the diagnostic APK, which has no network permission.

Android CPU samples and qemu instruction coverage also show that Android
enters native routines at `0xd966` and `0xdaf8` that are absent from the
Megler run. The sampling and coverage methods measure different things.
Together with the JNI sequence they give points to investigate in the
interpreter, but neither gives the reason for the branch or decodes the VM's
result. The offline comparison uses a stand-in for GMS's RuntimeApi and does
not test which result Google will accept.

**The VM responds to changes in its executable code.** Before running the
same ARM32 program, the offline APK compares the VM's executable segment
with the file. All 289,648 bytes agree on Android. The Megler run differs
at 556 bytes with ordinary patching and at four with
`no-plt-rewrite-libdB21738DAE92C.so`. Those four bytes are at `0x2e18`,
where Megler turns the VM's ARM `svc #0` (`0xef000000`) into a `udf`
(`0xe7f00af6`) so that Megler, rather than the host kernel, does the
Android call. Megler returns 2,228 bytes in both runs.

The same APK on the Mi 4c returns 34,018 bytes with the original code. It
returns 2,104 bytes with that exact replacement in place, while the
diagnostic's signal handler routes all eleven calls at that instruction
through Android's own syscall entry and returns their results to the VM.
When it installs the replacement and then restores the instruction before
running, the result is 33,920 bytes. An equivalent Thumb instruction at a
different VM address also reduces the result to 2,103 bytes. These controls
show that changing executable bytes is sufficient to shorten the VM's
collection, independently of Megler's syscall answers.

On ARM Windows Megler treats DroidGuard like its other libraries that
inspect their code. It leaves the PLT alone and puts the
syscall at a hardware breakpoint. The same Megler syscall handler does
the call and resumes after it. The qemu host has no hardware breakpoints
for guest code and still uses the patched instruction.

A fresh GmsCore 26.43.30 exchange fetched VM `276FB66A...`,
`libd4B1BB84DDE6C.so`. Its ARM syscall stub still has `svc #0` at
`0x2e18`. Both libraries export the same DroidGuard `initNative` and
`closeNative` JNI functions. The loader now identifies them by those
two defined function exports in the ELF itself, so a new library name
does not re-enable patching. The Self-test checks the identification
using a fixture with both exports and a name of its own. It checks the
original instruction and zero PLT rewrites on a host with breakpoints,
the trap instruction on qemu, and the shim's result for both calls.

In a separate qemu diagnostic on the first VM, an external debugger
restored the one remaining changed instruction before JNI initialization
and used virtual breakpoints to route each syscall through Megler's
existing trap handler. The executable segment matched the file at the
start of the run. The same offline program returned 6,496 bytes,
compared with 2,227 in the patched control. All 212 syscalls went through
Megler. This is a test harness for the offline program and does not
exercise Windows' hardware breakpoint delivery.
The corresponding live GmsCore run verified the new full and check-in
responses and loaded the new VM. It stalled during initialization under
the debugger before sending a fast request.

The Android diagnostic package and its transfer files were removed after
the comparison.

**Lumia 650 with the original VM instructions.** Transit on 0.1.0.690,
using `pristine-libdB21738DAE92C.so`, reported one syscall site at a hardware
breakpoint and traced calls from `+0x2e18` through Megler. The previous log
had fast-refresh errors. The pristine run had none, although it does not
show whether a fresh fast program was accepted. The device-key request
still failed three times with the protobuf end-group error, including once
after a successful check-in.

In 26.43.30, `atvj.b` checks HTTP status only after the response future has
returned a parsed `jofy`. In the streaming path, `dmzh.call` reads the body
through `dmzc.a`, and `dmzd.kk` reports a parse failure as
`dmyc("Parsing Exception")`. The saved independent HTTP 400 response from
the earlier qemu experiment starts with HTML. Its first byte, `0x3c`,
looks like a protobuf end-group tag, so that response can account for the
same error. The first Lumia log did not include the HTTP status.
A later Transit run on the same Lumia showed three POSTs to
`/auth/devicekey`, each followed by HTTP 400 in
`CronetUrlRequest.j` and the same protobuf parse failure. GmsCore never
reached the step that writes the key.

Seven saved qemu request and header pairs had the same nonzero GServices
Android ID in the `device` header and protobuf field 2, and
`app=com.google.android.gms`. Both inspected GMS releases set field 6 to
`missing_token` in their own request builder.

In a further Lumia run, all three device-key token calls took just over
60 seconds (60.083, 60.024 and 60.031). Their encoded returns contained
an error marker. Each return was immediately followed by the device-key
POST and HTTP 400. GmsCore's `byxc.c` waits up to 60 seconds for this call
and encodes an error into the token on timeout. In a separate run on the
same build, the worker thread spent 136–141 seconds between entering
`DroidGuard.initNative` and returning from `DroidGuard.init`. Once that
thread got to `ssNative`, each of five calls completed within 13 ms.

Tracing the calls inside `DroidGuard` located the delay on the Lumia. A
`Socket` constructor for TCP loopback port 7810 spent 130.328 seconds
before the worker handled its exception. That `init` took 140.068 seconds
overall. The next `init` reached the same constructor within 155 ms.
The phone's old network self-test already recorded about 127 seconds
for a blocking connect to a closed loopback port. A different DroidGuard
run read about 186 MB from `GmsCore.apk` in 22 seconds, so that read alone
cannot explain the timeout. A successful device-key request with a
completed token still needs verification.

On the same Lumia, an isolated network-check package reproduced the
closed-port stall in both blocking and non-blocking mode. Both attempts
reached the test's five-second deadline. With the Winsock loopback
timeout change, the same checks returned `ECONNREFUSED` in 500 ms and
501 ms respectively. The test also exchanged HTTP with its own loopback
listener. All 19 network checks passed.

Transit with runtime 691 and GMS 26.43.30 on the same Lumia completed
the `devicekey` VM init in 17.4 seconds. The caller waited 41.0 seconds
in total and returned a nonempty token after `ssNative` finished, within
the 60-second deadline. The same attempt still received HTTP 400 from
`/auth/devicekey`, followed by `Parsing Exception` and reports that the
device key was absent. Check-in succeeded. The run logged no FindTiles
call, so the disappearance of Transit's weak-GPS banner does not
establish that Google supplied a Wi-Fi position.

After restarting Transit with that registration still present, GmsCore
read a nonzero Android ID before collecting the device-key signals.
The VM init took 14.8 seconds, and the token call returned after 44.6
seconds. The request again received HTTP 400 and the key was still
absent. This rules out a zero ID and the 60-second token timeout for
that attempt. It does not establish that the server accepted the
registration or the token.

The signed `devicekey` program responses captured in qemu have a
10-second TTL. That is shorter than the phone's two observed init
durations. The qemu run that submitted its request first with curl
formed the request 98 ms after capturing the signed program and
submitted it as soon as the capture appeared. It still received HTTP
400. The delay observed on the phone therefore cannot account for
all the rejections. A successful `/auth/devicekey` exchange on the
Android reference remains needed for comparison.

The Android 11 reference with GMS 26.26.34 already had a device-key file.
A refresh assembled a new token of 8076 base64 characters in 265 ms,
then built the protobuf with a nonzero device ID and the literal
`missing_token`. GMS's own Cronet failed with `ERR_CERT_INVALID` in the
exception chain, before an HTTP result was available. The diagnostic
advanced the retry in memory and left the existing key file in place.
This run gives a reference for request construction, but no HTTP control
for the Lumia's 400.

The same Android reference then made one new device-key request using
Android's standard HTTPS client with certificate verification. The
diagnostic forwarded GMS's original headers and 8129-byte protobuf body
once. Its 8078-character token took 295 ms to collect. Google returned
HTTP 400 and a 1664-byte body. The diagnostic preserved the existing key
file and recorded only status and lengths. This reproduces the rejection
outside Megler with a substantially larger token. It does not establish
that a native Cronet request from a stock Android device would fail.

With the owner's approval, the same reference sent a new original request
through GMS's own Cronet 149.0.7827.159. For the connection to
`android.googleapis.com` alone, a temporary native hook enabled
`SSLConfig::ignore_certificate_errors` while the certificate result was
processed, then restored its previous value. The checked function bytes
and object offsets came from that exact ARM64 library. The token took
338 ms to collect. Cronet completed TLS and reported HTTP 400 over
`h2`; GMS then raised `Parsing Exception` caused by a protobuf end-group
error. The existing device-key file remained present. The hook was
detached and the GMS main process restarted to discard the pooled TLS
connection. Thus the rejection also occurs through native Cronet on this
rooted Android reference. A successful stock-device control is still
absent.

For 26.43.30, `trace-calls-atvj` identifies the endpoint and
`trace-calls-alkl` shows the elapsed time and return of the token call.
With Cronet 157,
`trace-calls-org.chromium.net.impl.CronetUrlRequest` shows the status
at `j`; `trace-calls-m1.ic` shows it in the response constructor. Match
the status to the request and parser failure. Keep only metadata from
these traces, since they can include token prefixes.
Use `trace-calls-within-com.google.ccc.abuse.droidguard.DroidGuard` to time
the loopback probe inside `init`. Check for the socket exception well before
the 60-second deadline, then for a device-key token without the encoded
error, the response status and the stored key.

**Draft module assessment (2026-11-15).** The supplied
`PlayIntegrityFix_v19.1.zip` is 127130 bytes, SHA-256
`bbc618e794d8bb11add4d8f54c8ed53dfee5c6df07b49813bcbd5154c0c89c7c`.
Its module metadata names chiteroman and version 19.1. The installer and
boot scripts were read, its DEX was decompiled, and the native libraries'
imports, exports and strings were inspected. This identifies the local
sample; it does not authenticate it as an upstream release. All analysis
files are in ignored build folders.

The archive contains ARM32/ARM64 Zygisk libraries, a Java DEX, root
installation/boot scripts and a WebUI. Its bundled profile describes a
Pixel 6 Android 16 beta with security patch 2025-04-05. The Action script
downloads Android release/OTA metadata and writes a replacement profile.
The boot scripts change build/debug/boot properties, adjust Magisk's
DenyList and change access to SELinux files on a permissive Android
system. These scripts depend on Android root/module infrastructure.

Its Java payload has three relevant mechanisms. It changes configured
`Build`/`Build.VERSION` fields, wraps the AndroidKeyStore provider, and
optionally replaces the platform package's reported signing certificate.
The wrapper delegates key operations except
`engineGetCertificateChain`: a caller whose stack contains
`droidguard` receives `UnsupportedOperationException`. That suppresses
the chain a working keystore would otherwise return. It does not
generate an attested key or obtain an AppCertManager device key.

The current PlayIntegrityFork was inspected at
`d7e6a4d635b7463bbee7c9701f91ebfc6f5eb4ff`; the latest release reported
by GitHub on this date was v19. Its native code scopes configuration to
the GMS unstable process and, for separate options, Play Store.
It hooks `__system_property_read_callback` with ShadowHook and injects
Java code for typed Build-field changes. The provider wrapper retains the
DroidGuard certificate-chain exception. Defaults enable Build, property
and provider changes; platform-signature and Play Store overrides are
optional. The repository supplies a configuration template, so a working
device profile is another input to an experiment.
[PlayIntegrityFork native code](https://github.com/osm0sis/PlayIntegrityFork/blob/d7e6a4d635b7463bbee7c9701f91ebfc6f5eb4ff/app/src/main/cpp/main.cpp),
[provider wrapper](https://github.com/osm0sis/PlayIntegrityFork/blob/d7e6a4d635b7463bbee7c9701f91ebfc6f5eb4ff/app/src/main/java/es/chiteroman/playintegrityfix/CustomKeyStoreSpi.java),
[configuration template](https://github.com/osm0sis/PlayIntegrityFork/blob/d7e6a4d635b7463bbee7c9701f91ebfc6f5eb4ff/module/example.pif.prop).

TrickyStore's current release documentation was inspected at
`55d65f305e04110b87210d86cc12b64a1b7e151d`; its latest release was
1.4.1. The project states that releases from 1.1.0 are closed-source.
The last public tag, 1.0.3 at
`6f82e4011b770e390d1157928c6cf45df99b7d7d`, was read as an architectural
baseline. Its injector and Binder interceptors reach Android keystore2.
They can replace a returned certificate chain or intercept challenged
key generation and synthesize key metadata and a chain. The certificate
builder uses the configured keybox's private key and certificates to
sign the generated leaf and rewrites attestation data including
RootOfTrust. Current documentation also describes fallback generation
and configurable patch levels; the closed release's implementation was
not audited.
[Current TrickyStore documentation](https://github.com/5ec1cff/TrickyStore/blob/55d65f305e04110b87210d86cc12b64a1b7e151d/README.md),
[open generation interceptor](https://github.com/5ec1cff/TrickyStore/blob/6f82e4011b770e390d1157928c6cf45df99b7d7d/service/src/main/java/io/github/a13e300/tricky_store/SecurityLevelInterceptor.kt),
[open certificate builder](https://github.com/5ec1cff/TrickyStore/blob/6f82e4011b770e390d1157928c6cf45df99b7d7d/service/src/main/java/io/github/a13e300/tricky_store/keystore/CertHack.java).

For Megler the relevant boundaries are:

| Mechanism | Existing Megler behaviour | What a useful experiment would establish |
| --- | --- | --- |
| PIF's certificate-chain exception | A challenged `PairGen.generateKeyPair` fails with `KeyStoreException(-100)` before a new chain exists. Ordinary chain retrieval is implemented. | Whether the failing device-key program actually reaches chain retrieval and changes its request when that call throws. |
| Build/property profile | Java Build fields and native properties already describe a synthetic Android device. They are shared within the app's process. | Whether one consistent profile, scoped to hosted GMS, changes Google's reply with the same GMS version and transport. |
| Platform-signature replacement | `PlatformSigner` reads the framework-res APK's certificate. PIF's optional replacement is another platform identity. | Whether that reported identity is a relevant input to this particular rejection. |
| TrickyStore key generation and chain rewriting | Megler implements its keystore directly in Java, without Android's keystore2/KeyMint services. Software-attestation experiments above still received HTTP 400. | Whether a specified, verifiable attestation chain changes the server result while keeping generated keys and certificates consistent. |

The attestation keybox signs Android key-attestation certificates.
AppCertManager's `device_key` is the response from Google's
`/auth/devicekey` service and supplies material for Spatula signatures.
One does not supply the other. Neither inspected project's code
demonstrates a successful device-key exchange inside Megler.

The draft now implements PlayIntegrityFork's three default mechanisms as
an opt-in `megler.IntegrityProfile`, applied before `Application.onCreate`.
It reads the first existing file in the Android app's internal data directory
(`Native.dataDir()`, the directory returned by `Context.getDataDir()`):
`custom.pif.prop`, `custom.pif.json`, `pif.prop`, then `pif.json`.
For Transit in its Windows package, this is
`LocalState\apps\com.thetransitapp.droid\data\custom.pif.prop`.
A profile belongs to the client app hosting GMS; a profile in a separate
GMS app's data directory does not configure Transit. Restart the client
process after installing, changing or removing it.

The UTF-8 property format accepts `name=value` lines, blank lines and `#`
comments. JSON accepts an object with string or scalar values. Files are
limited to 64 KiB. The three switches default to `1` when a file exists:

```properties
spoofBuild=1
spoofProps=1
spoofProvider=1
# Add the chosen profile's Build fields and explicit system properties here.
# DEVICE_INITIAL_SDK_INT=25
# ro.board.first_api_level=25
```

`spoofBuild` applies named scalar fields of `Build` or `Build.VERSION`
using their declared types. `spoofProps` applies explicit property names
containing a dot to both Java `SystemProperties` and Megler's native
property table, which serves bionic's property get/read/callback APIs.
When both switches are enabled, common Build identity fields are also
mirrored to their corresponding properties; explicit property entries
win over those inferred values. Property values must fit 91 UTF-8 bytes.
Unknown Build fields, invalid field types and unsupported options reject
the profile before it is applied. Empty values are ignored.

`spoofProvider` makes `AndroidKeyStore.getCertificateChain` throw
`UnsupportedOperationException` when a class name in the calling stack
contains `droidguard`, case-insensitively, as in the upstream wrapper.
Other callers keep their ordinary certificate chains. It does not change
challenged key generation, generate an attestation chain, or obtain a
Google device key. With no profile, all three mechanisms are inactive.
The startup log reports field/property counts and whether the chain block
is enabled, without logging profile values.

There is one significant scope difference from Zygisk: Build fields and
system properties affect the entire opted-in app process, including its
client code, because Megler hosts GMS in that same process. The provider
exception is restricted by the caller's stack. This draft does not
implement wildcard properties, `spoofSignature`, `spoofVendingSdk` or
`spoofVendingFinger`; enabling those optional upstream modes rejects the
profile. It supplies no fingerprint and downloads no replacement profile.
It does not install or execute the module's root scripts or native hooks.

The data test app covers missing configuration, property and JSON parsing,
typed fields, Java/native property agreement, the scoped chain exception,
invalid input preserving the previous values, and disabled modes. These
checks establish local behaviour. To establish usefulness for devicekey,
compare a fresh request with and without a chosen profile, then verify an
HTTP success, a stored key loaded at the next start and a Google
network-location result. The earlier native Cronet control on Android also
returned HTTP 400, so that response alone cannot identify a Megler-specific
API defect. No successful server exchange with this profile has yet been
observed.

Validation (2026-11-15): `tools/build-tests.sh arm` and
`tools/build-tests.sh win`, with the shared heavy-build lock and
`MEGLER_JOBS=4`, each completed with 33 result groups and zero failed
checks. ARM's main passes reported 9187 and 9346 checks; Windows's 8614
and 8736. Both data-app runs passed `integrity-profile`; the Windows run
included the cached ANGLE build.

### HP comparison, 2026-11-15

The Elite x3 had Megler 711 and GmsCore 26.41.31 (`264131009`), unlike
the Lumia's 26.43.30. An isolated test branch based on the source of 711
(`6a6d1caf`) applies the profile draft while retaining its newer shared
keystore implementation. The experiment used test runtime 713.

A small, separate diagnostic app, `megler.test.hpdevicekey`, binds the
hosted DroidGuard service and calls this GMS version's
`appr.a(Context).b()` off the main thread. It installs no interception
hooks. Its own class names contain no `droidguard`, so they cannot
accidentally satisfy the profile's certificate-chain stack check.
With the profile disabled, the service bound and the device-key call
returned `false` after 37.5 seconds. AppCertManager logged a parsing
exception. No DGFRR error occurred in the captured log. This establishes
neither a valid VM token nor this request's HTTP status: those were not
captured. The raw log stays in ignored private build output.

After charging, the paired profile-enabled run used the four identity
fields from the supplied PIF 19.1 archive (Pixel 6 beta fingerprint,
security patch 2025-04-05). The startup log confirms four Build fields and
four native/Java properties applied, with the provider mode enabled.
The trace records `engineGetCertificateChain` ending with
`UnsupportedOperationException`, establishing that the stack-scoped
provider path actually ran. The device-key call returned `false` after
59.7 seconds and AppCertManager again logged a parsing exception. The
request's HTTP status and the VM token's validity were not captured.
This particular profile did not obtain a key; newer profiles remain
untested. The diagnostic changes no protocol messages or TLS validation.

The test-only enable flag and the app's `custom.pif.json` have been removed,
and its process was stopped. Normal package 714, without an auto run, is
installed on HP. The inherited `ui-update-all` source was removed from
both LocalState and `Pictures/Megler/diag/megler/flags`. Its Pictures copy
had previously recreated the LocalState flag after deletion there.
Windows' installed-package list confirms that all 42 client packages
which received test runtime 713 now use normal runtime 714, with no test
713 package remaining in that list. Updates preserved their data and were
requested individually. The three packages still using 711 were outside
the update list. Final filesystem checks confirm that the profile,
test flags and mirrored Update All source are absent; the main package
retains only its pre-existing `keep-screen-on` flag.

On this newer source base, the full ARM suite passed 33 result groups
(main passes 9215 and 9374 checks). The full Windows retry passed 33
result groups (8639 and 8761 checks). Every final group reported zero
failures. The first Windows run failed its intermittent shell check
"asked again, Megler's other settings kept" once; no runtime code changed
before the successful full retry. Both data-app profile runs passed.

### Supplied 4PDA module set, 2026-11-15

The two supplied screenshots describe Reddit login on rooted Android,
using Play Integrity Break's intervention, synthetic error and synthetic
response settings. The second post adds SUSFS kernel-version presentation
for a kernel whose name contains `WILD`. The live forum pages could not
be fetched; their text was read from the supplied screenshots. These are
claims about a Reddit login flow, not a demonstrated `/auth/devicekey`
exchange, a Play Integrity verdict or a Google Wi-Fi fix in Megler.
[First post](https://4pda.to/forum/index.php?showtopic=738196&view=findpost&p=145071657),
[follow-up](https://4pda.to/forum/index.php?showtopic=738196&view=findpost&p=145989144).

Each supplied archive was copied to its own ignored analysis directory,
hashed, checked for traversal/symlinks and unpacked without executing its
installers or Android binaries on the host. Static DEX analysis used jadx.
The PIF DEX decompiled without reported errors; PIB, TrickyStore service
and LSPosed framework reported 20, 10 and 2 decompiler errors respectively.
This is a partial static audit, not a proof of all native behaviour.
The module README and script instructions were treated as sample contents.

| Supplied file | SHA-256 |
| --- | --- |
| `Zygisk+Next-v1.5.0.zip` | `474d58abc208c0779e7f8f1d8449db755a874d475c13b9cf8decf74bc171933b` |
| `Tricky-Store-v1.4.1-248-3b07ee3-release.zip` | `0bc227c59b9b4baa77d8c46ff310c276a88114e4dcf7ed10d886023924183454` |
| `Play_Integrity47.zip` | `06a93bbbcbe4d94fb5dc2d6e1e651684a74c1568973984e8c118a70c26ac7ec3` |
| `ksu_module_susfs_1.5.2+.zip` | `abcd6bf456ea8189298d9e496faa440201d90e4f0bd93b83c0527589001c9376` |
| `pib-64-64.apk` | `f9c69912a37123398a2c2602fb6d1a5e1420cff4f2ff93b49da2d6dd30dc70d4` |
| `LSPosed-v2.2.0-7854-release.apk` | `74d89370b4b17a94c130089b2868c3d6f289ab8c5c5715ba43a909d4bd6e216b` |
| `LSPosed-v2.2.0-7854-release.zip` | `725915707f88a4f5ebecddab908cb217aa0a6d98955db5885bc9fadda81e75b8` |

- **Play Integrity Break 64**, package `it.eldavo.pib`, versionCode
  `7304742`: `assets/xposed_init` names `XposedEntry`. Its hook is installed
  in the main Play Store process. `deliverSyntheticBlockedResponse`
  invokes the callback with a Bundle containing `error` and
  `is.error.remediable`, and the original request is short-circuited.
  The supplied configuration defaults to `-8` and `true`. This is
  `TOO_MANY_REQUESTS`, not a token or successful integrity verdict.
  It can also suppress delivery or delay it, log events and optionally
  send telemetry. Those extra modes are not part of this Megler change.
- **Play_Integrity47**: metadata names `Play Integrity Fix [KEYBOX]` v47,
  rather than the previously examined chiteroman 19.1 sample. Its Java
  provider has the same DroidGuard-stack chain exception. Its template
  adds wildcard properties; boot scripts change root/boot properties.
  Its installer backs up/removes an existing TrickyStore keybox, chooses
  a bundled `*.@fateh7` file and copies that to TrickyStore. Its profile
  updater obtains Android build metadata using wget with certificate
  verification disabled. None of those scripts was run.
- **TrickyStore 1.4.1**: the bundle supplies `service.apk`, native libraries,
  target list and a default software keybox (two private keys and four
  certificates). No key contents are recorded here. Its installer calls
  it a software keybox; presence in the ZIP proves neither hardware
  provenance nor acceptance by Google. The current service/native
  implementation is not fully audited. This port does not import it or
  fabricate an attestation chain.
- **Zygisk Next 1.5.0**: a root/Zygisk injection runtime for Android's
  init/zygote processes. Megler has neither Android init/zygote nor ART.
  It is not an integrity-result provider. No binaries are redistributed.
- **LSPosed 2.2.0 (7854)**: ART hooks supplied by LSPlant plus a daemon and
  manager. The separate supplied manager APK exactly matches the ZIP's
  `manager.apk`. Megler already has its own Xposed method-hook primitives
  for Aliuhook; that does not establish compatibility with the entire
  LSPosed daemon, its ART implementation or its manager.
- **SUSFS 1.5.2-R28**: root hiding controls for a SUSFS-patched Linux kernel
  through `ksu_susfs`. The shown kernel setting calls `set_uname` with the
  chosen release/build. Megler already supplies a synthetic Linux `uname`
  and `/proc/version` (`4.4.0-megler`), with no `WILD` suffix. This does not
  prove other environment checks pass; installing KernelSU scripts cannot
  change the Windows kernel.

The upstream PIB README explicitly limits its mechanism to applications
that tolerate integrity errors; it does not obtain BASIC/DEVICE/STRONG
verdicts or supply a working keybox.
[PIB project explanation](https://github.com/ElDavoo/PlayIntegrityBreak/blob/pib/README.md),
[Google's error codes](https://developer.android.com/google/play/integrity/reference/com/google/android/play/core/integrity/model/IntegrityErrorCode).

#### Opt-in PIB error response in Megler

The runtime now provides the supplied PIB's default synthetic blocked
response without requiring its injection stack. Place a file named
`play-integrity-break` in the selected app package's `LocalState/flags`
(or `Pictures/Megler/diag/<Android package>/flags`), then restart that app.
A same-named marker in its Android internal data directory is also accepted
for test harnesses. This is independent of `pif.json` and applies only in
the selected client process.

Classic integrity-token requests, standard warm-up and standard-token
requests return a single callback with `error=-8` and
`is.error.remediable=true`. No token is returned and no request reaches
Google through these local Play Integrity endpoints. With the switch off,
Megler retains its previous `API_NOT_AVAILABLE` (-1) response. Age Signals,
GMS DroidGuard requests and `/auth/devicekey` are unchanged. An application
or its server decides what to do with either error.

The data-app regression exercises all three transactions in default,
PIB-enabled and restored modes, checks the callback descriptor/code,
remediability and absence of a token, and removes its test marker.
Full validation on this draft: ARM and Windows each reported 33 result
groups and zero failed checks (main passes 9215/9374 and 8639/8761).
`integrity-callbacks` passed both data-app runs. Heavy work was serialized
under `heavy.lock` with `MEGLER_JOBS=4`.

This implementation is a protocol-behaviour counterpart, not a port of
all seven supplied packages. Reddit login, device-key issuance and Wi-Fi
positioning with this change have not been demonstrated on a phone.

#### Claude login comparison on HP

The owner installed `com.anthropic.claude` on HP and repeated the same
email sign-in action. On its original runtime 714 the log records one
classic token request answered `API_NOT_AVAILABLE`; the UI asks for a
Google Play update or a Google account. Normal runtime 716 carries this
Java change, has no auto run, and was installed as a compact package with
optional bundled Google APK installers omitted. The already installed
Google apps and their data were retained. Only Claude's client package
was updated; no Update All command was used.

With `play-integrity-break` enabled only for Claude, its log records two
classic token requests answered `PIB synthetic error -8, remediable`, and
zero old API_NOT_AVAILABLE replies. The UI changes to an app-verification
failure and says the sign-in email could not be sent. There is no fatal
exception in the captured run. Thus the PIB mode executes on HP, but its
default response does not unblock this Claude login flow. It does not
establish what all other PIB settings or applications would do, and it
provides no device-key result. The experimental flag was removed after
the comparison; Claude's data was not cleared. Raw logs and screenshots
containing the owner's address stay out of commits.


### Draft keybox-backed attestation

A separate opt-in `megler.KeyboxAttestation` supplies the generation path
that the module set uses when Android's keystore cannot attest. It loads
an external signing key and certificate chain and signs the generated
key's leaf certificate. This is software generation inside Megler, not a
Windows TEE or an implementation of the closed-source TrickyStore 1.4.1
binary. Google acceptance remains a separate test.

Configuration belongs to the Android client app hosting GMS, in its
internal data directory (`LocalState/apps/<package>/data`):

```
attestation/enabled
attestation/keybox.xml
attestation/config.properties       # optional
```

The marker enables the path; no keybox or private key is bundled into
Megler's source or releases. `keybox.xml` accepts one AndroidAttestation
keybox with one or two EC/RSA signing entries. Private keys may be PKCS#8,
RSA PKCS#1 or EC SEC1 PEM; certificates are X.509 PEM. Inputs are bounded
(1 MiB XML, 16 KiB settings), and DTD/entity declarations are refused.
The chain's validity dates, CA/path-length and key-usage constraints,
issuer names and signatures are checked, as is a sign/verify proof that
the private key matches its first certificate. Revocation and Google's
trust in the issuer are not established by these local checks.

For a challenged EC/RSA key generation the leaf carries the newly
created public key, exact challenge, requested subject/serial/dates,
key usages and Android extension `1.3.6.1.4.1.11129.2.1.17`. Its application
ID contains the actual caller package/version and SHA-256 signing
certificate digests; hosted GMS uses its own context. Authorizations
include purposes, algorithm, actual key size/curve or exponent, requested
digests/paddings and validity dates, no-auth, creation time, generated
origin, OS version and patch level. Device properties are included only
when requested. The complete chain is persisted with the key through the
existing app/shared-GMS keystore. An invalid configuration or failed
certificate construction does not replace an existing key.

A configured `setAttestKeyAlias` selects an existing local signing alias
instead of the external keybox. A key created with PURPOSE_ATTEST_KEY
alone receives CA and certificate-signing constraints so its descendants'
chains can be checked. StrongBox, authenticated keys, unique IDs and
serial/IMEI-style ID attestation requests remain unsupported and are
refused, rather than silently downgraded. Without the enable marker,
Megler retains its existing unimplemented-attestation exception.

The default extension reports Software (0), with no fabricated TEE
RootOfTrust. For an explicit TrickyStore-style experiment,
`config.properties` accepts `securityLevel=1` and requires both
`verifiedBootKey` and `verifiedBootHash` as 32-byte hexadecimal values.
`deviceLocked` defaults to false and `verifiedBootState` to Unverified
(2); these are configurable claims, not measured boot evidence from
Windows. `osVersion` (e.g. 140000), `osPatchLevel` (YYYYMM),
`vendorPatchLevel` and `bootPatchLevel` (YYYYMMDD) may be set explicitly.
Otherwise OS version/patch reflect the current Build values. Applications
must not interpret these experimental claims as physical hardware backing.
If a PIF profile's `spoofProvider` is also enabled, its DroidGuard chain
exception still applies; disable that mode to test chain consumption.

The metadata-only flag `trace-attestation` logs requested algorithm,
challenge length and whether keybox mode is active, without logging
challenge bytes, alias names, keys or certificates. Successful generation
logs only algorithm, chain length, challenge length and configured level.

Static inspection of the supplied files found that the default TrickyStore
EC issuer expired on 2026-01-08; its RSA issuer remains valid to 2035.
The separate keybox embedded in PIF47 has matching EC and RSA private keys
and chains which validate locally, with leaf issuer certificates valid to
November 2028. A separate check of Google's public revocation list on
2026-11-15 identifies both PIF47 signing certificates (EC and RSA) as
`REVOKED`, reason `KEY_COMPROMISE`. Only the public list was downloaded;
private keys and device/user data were not submitted. Consequently this
sample can validate local import/signing mechanics, but is unsuitable as
an unrevoked identity for a server-acceptance test. The runtime's offline
chain validation does not query this list automatically. Sample material
stays in isolated ignored build directories.
[Google attestation certificate status](https://android.googleapis.com/attestation/status).

A fresh upstream audit on 2026-11-15 pinned osm0sis/PlayIntegrityFork main
at `d7e6a4d635b7463bbee7c9701f91ebfc6f5eb4ff` (2026-11-13). Neither its
tracked files nor the official v19 release ZIP contains a keybox or a PEM
private key. The ZIP SHA-256 is
`83b4d5f32743805c5b5a69361d73b8ab9c095325e24f20711afa1abea837b58f`.
The recent `a645788` change adds `ro.board.first_api_level` when missing;
it does not issue or renew attestation certificates. `autopif4` downloads
public Pixel build profiles and adjusts TrickyStore patch configuration,
not keyboxes. Its RKP warning requires a separate keybox for some newer
profiles. Upstream's README likewise distinguishes its property spoofing
from TrickyStore with an unrevoked hardware keybox; the supplied PIF47
archive is not the official osm0sis v19 package.
[Property change](https://github.com/osm0sis/PlayIntegrityFork/commit/a645788294d35df755efc38ae669836f26b63177),
[v19 release](https://github.com/osm0sis/PlayIntegrityFork/releases/tag/v19).

A further audit pinned Falcon Integrity Fix at `0c12258` (2026-11-13).
Its v3.30.2-tagged ZIP (`FALCONINTEGITY_FIX_V3.30.zip`, SHA-256
`35d2d3772b14064341e0ec7f24c004c1a0738a6acbfc119d4d5bb500c5522505`)
does contain a different keybox. The XML SHA-256 is
`a71ab688e1ce8fc93758231bebfe579691971c164af17b31c8379e418b4a1e5a`.
It has one matching EC signing key and four certificates, whose chain
verifies locally and expires first on 2026-11-25 at 05:44:23 UTC. Its root
matches Google's published new Key Attestation CA1 root. None of its four
serials occurs in Google's list downloaded and refreshed on 2026-11-15.
This is a current local validation result, not a server verdict.

TAKR means Tricky Addon Keybox Repository. Its public API lists entry 41
as hardware/RKP, EC, four certificates, uploaded 2026-11-13, nearest
expiry 2026-11-25T05:44:23Z. Those metadata match Falcon's candidate, but
no byte-for-byte identity was established with the TAKR download. The
number is a repository entry, not an Android API or module version.
Expiration is an upper limit, not protection against an earlier revocation.

Falcon's `service.sh` checks through `kb_sync.sh --auto` after boot and
every 3600 seconds by default (minimum 900). Its configured primary source
is davidepalma.it/pib/keybox.xml; fallback is TrickyStoreOSS's module XML.
Those endpoints do not currently return the bundled candidate: the primary
has a revoked RSA intermediate and the fallback has the expired AOSP EC
issuer described above. Thus a fresh bundled keybox does not prove that
unattended replacement will find another valid one. The downloader uses
curl -k / wget --no-check-certificate; that transport behavior was not
copied into Megler. No downloaded module code was executed.

The original evoker0/AlwaysStrong repository returned 404. The inspected
BloodCorrupt/AlwaysStrongFork at `54c2a70` bundles TEE interception and PIF,
with configurable periodic fingerprint/keybox refresh. Its keybox fetcher
uses a project mirror (default plain HTTP evoker.qzz.io/key), detects changes
by a locally computed hash and preserves the old file on fetch failure.
It downloads replacement material; it does not mint Google identities.
TrickyStore/TEE interception consumes that material to sign chains; PIF
supplies the build profile. These are separate mechanisms. The supplied
4PDA page could not be retrieved directly during this audit.

A separate diagnostic APK loaded the Falcon candidate in ARM/qemu and
verified five-certificate chains for newly generated EC and RSA keys in
both software and explicitly configured experimental TEE modes. Each
local-only run reports 5 checks, 0 failed and no Google request. A further
fresh-data GMS 26.41.31 probe used the experimental mode: metadata shows
GMS requesting attestation and successful five-certificate generation for
its challenges. AppCertManager still reports `dito: Parsing Exception`
and `Device key obtained=false`. The log has no ERR_CERT_INVALID or old
DGFRR byteCode error. This run did not capture the device-key HTTP status,
so the earlier HTTP 400 finding must not be assumed for this exchange.
The guest harness reports 5 checks, 0 failed; that only validates the
harness, not device-key issuance. No phone was modified for these probes.

A follow-up fresh-data control enables CronetUrlRequest, appr and DroidGuard
call tracing. It observes the actual `/auth/devicekey` request followed by
CronetUrlRequest.j HTTP 400 over h2 and the same `dito: Parsing Exception`.
Other HTTP 200 responses in that run belong to earlier requests, not a
successful device-key exchange. DroidGuard init succeeds and returns
nonempty signal data; the old byteCode and TLS-certificate errors are absent.
Thus the externally supplied candidate fixes neither the server refusal
nor device-key issuance. No validity verdict for the signal token follows
from its nonzero length or successful local generation.

Static analysis of this exact release's appr.b also explains an early false
return in the diagnostic: a process-local elapsed-time throttle precedes
token collection and the network call. A fresh-process call may return its
initial false result without sending anything; a later background call
made the request above. The manager builds a protobuf with Android ID,
session, DroidGuard result, SDK and GMS version, and explicitly sets the
`missing_token` field itself. It does not directly append the keybox chain
to this request. Attestation can influence collected signals but replacing
the XML alone does not establish what Google accepted. Request values,
token bytes and raw traces remain in ignored diagnostic folders.

A corrected pass-through capture of asgv.b retains the serialized request
immediately before the original transport call. Static hook callbacks have
no receiver slot; earlier private diagnostic attempts used the instance
convention and are excluded from protocol evidence. The corrected run
changes only the diagnostic manager's in-memory retry timestamp to ensure
an attempt within the test window; it does not replace request fields,
headers, token or TLS verification. The hook itself may affect VM signals.

The captured request is 12142 bytes: field 1 is a 12091-byte base64 token,
fields 2 and 3 are nonzero Android ID and session integers, field 4 contains
SDK 34 and GMS version 264131009 (both fixed32), and field 6 is the literal
`missing_token` set by GMS's builder. The header `device` agrees with field
2, `app` is com.google.android.gms, and Content-Type is application/x-protobuf.
The remaining header names are gmscoreFlow and gmsversion. No values of
identifiers or token material are retained in this document.

The token base64 decodes to a syntactically valid 9068-byte protobuf with
length-delimited fields 1/2/3/5 of 6/9012/25/16 bytes. It contains no literal
ERROR, Exception or TimeoutException markers. This rules out an absent
token or a plain encoded exception for this attempt; it does not prove
that Google accepts the opaque signal payload. The original transport
continues and AppCertManager again reports Parsing Exception and no key.
The control harness ends with 5 checks, 0 failed. No production code was
changed and no phone was updated for this analysis. The server's specific
reason remains unknown; empty token, zero/mismatched ID and wrong public
GMS version are not supported explanations for this captured attempt.

A combined control applies Falcon's Pixel 6a Canary profile with the new
keybox and spoofProvider=0. The startup log confirms 13 Build fields and
14 process properties, certificate-chain block false. Wildcards are
expanded only to the explicitly supported build-id, system patch and
product/board first-API properties; unsupported Vending modes are omitted.
This is a controlled Megler port of the applicable profile, not execution
of Falcon's Android module or all of its global resetprop operations.
The captured request is 6929 bytes with a 6879-byte token (5159 decoded),
nonzero matching ID and no plain error markers. AppCertManager still
reports Parsing Exception and false. The earlier mislocated profile file
was not applied and is excluded from this combined-control evidence.

The same run reports check-in SUCCESS before the captured device-key
request. Its request ID matches the persisted GSF GServices android_id
(read-only equality comparison; the value is not exported). A further
process restart on the same registered diagnostic database retains the
profile/keybox and obtains a fresh token. Cronet explicitly reports HTTP
400 and the key is still absent. This excludes merely waiting for the
first check-in as a sufficient remedy for this control. Both completed
controls report 5 harness checks, 0 failed, not a device-key success.

A true positive reference exchange is still absent. No Android reference
was connected over ADB at the end of these controls; an Android without
root cannot expose GMS's private device-key file or decrypted HTTPS call
through an ordinary diagnostic APK. External app success can also use an
existing key, so it must not be reported as a newly observed issuance.
No additional runtime change, phone update, merge or push follows from
these negative controls.

On the explicitly selected Android 11 Mi 4c, a fresh baseline through
verified Android HTTPS again receives HTTP 400 with 1664 response bytes.
A Java-only normal-Cronet call first gets ERR_CERT_INVALID. A later
certificate-diagnostics-only Cronet control, with all certificate override
code removed, establishes HTTP/2, cert_status=0 and a known root, then
receives HTTP 400 and the protobuf end-group parsing exception. This is
a stronger current transport control than the older certificate-bypass
experiment. A separate shell-UID HTTPS probe sees Google's public WE2 /
GTS Root R4 chain and no HTTP/HTTPS proxy configured; this does not establish
that every possible network intermediary is absent.

With separate explicit permission, a temporary GMS-only TrickyStoreOSS
engine from Falcon and the audited new keybox were installed on Mi 4c.
A fresh test-owned GMS alias receives five certificates, whose signatures
and dates validate through the published new Google root and whose leaf
has the attestation extension. The alias is removed afterward. A fresh
verified-HTTPS device-key control still receives HTTP 400. The first
attempt times out without status and is inconclusive; the bounded retry
gets 400. The original device_key is backed up only on the phone, compared
byte-for-byte before rollback, and retained unchanged.

A further explicitly approved control adds only the official PIF v19
classes.dex and arm64 Zygisk core with Falcon's profile. It executes none
of Falcon's installer, updater, conflict-removal or global resetprop
scripts. Vending/signature/provider-blocking modes are off. Native PIF
startup confirms config/DEX reads, the property callback hook and
EntryPoint.init; Java-side logs name MODEL, FINGERPRINT and SECURITY_PATCH.
The GMS-owned attestation probe again validates the new five-certificate
chain. No Frida session is attached to the DroidGuard/unstable process in
this native-module control. The request is triggered and observed from
GMS's main process. It receives HTTP 400, including once over HTTP/3.
This is not a successful device-key control despite native PIF loading.

Both temporary-module controls were rolled back and rebooted. The three
session-created module/config directories are verified absent, the engine
is no longer running and the original key is present; its bytes matched
the on-phone backup immediately before removal of that backup. Existing
root modules, denylist settings and account data were not changed. An
initial remote-exit-code check incorrectly inferred a disable marker;
explicit marker/file checks corrected that and it is not protocol evidence.

Primary microG source at f686900c68aa84d1605b52304f6615f3593c6f50 identifies
request field 6 as a GCM registration token, obtained with DeviceKeyRequest
scope, unlike the literal in these Google GMS builds. An on-phone,
verified-HTTPS diagnostic using its own check-in credentials receives
`Invalid scope` for DeviceKeyRequest. Standard GCM scope succeeds and
returns a 131-character registration token. Replacing only field 6 in one
fresh original device-key protobuf with that token still gets HTTP 400;
no original key is replaced and no credentials or token bytes leave the
phone. Thus this protocol variation supplies neither a fix nor proof that
Google's literal is malformed. GCM registration succeeds but device-key
issuance does not; these outcomes must remain distinct.

A further reference uses the supplied Google Translate
10.43.74.1015708864.6-release, versionCode 701119201, ARM32/minSdk 26.
`apksigner verify` accepts its signature; the signer SHA-256 matches the
published Translate certificate,
`3d7a1223019aa39d9ea0e3436ab7c0896bfb4fb679f4de5fe7c23f326c8f994a`.
The [version listing](https://www.apkmirror.com/apk/google-inc/google-translate/google-translate-10-43-74-1015708864-6-release-release/)
provides the matching fingerprint.
Translate can return a correct translation without obtaining Spatula:
its `SpatulaAuthInterceptor` waits four seconds for the auth client, then
continues without X-Goog-Spatula on interruption, failure or timeout.
Translation success therefore does not establish device-key issuance.

With separate explicit approval, the old device_key was temporarily
renamed within GMS's files directory, GMS and Translate restarted, and
Translate given fresh synthetic text. No Frida session was attached to
any GMS process or the VM during this native-client control. The repeated
PIF/TrickyStore setup uses the same approved files and scope described
above. A 100-second live observation confirms PIF's native property hook,
EntryPoint.init and Java Build spoofing, plus the TrickyStoreOSS process.
Both fast refresh and AppCert report ERR_CERT_INVALID; no new device_key
file appears. This run does not establish an HTTP refusal because TLS
fails first. An earlier attempt did not confirm PIF logging: its diagnostic
toggle missed a CRLF line ending, and a tail-limited log snapshot was
insufficient. Only the live observation is used for the PIF loading claim.
After each attempt the original key is restored and compared byte-for-byte
with the on-phone backup. All three temporary module/config directories
are removed, and the rebooted phone is checked for the original file,
absence of the renamed file and absence of the temporary engine.

The actual AppCertService in this Android GMS APK runs in
com.google.android.gms.persistent, while DroidGuardService runs in
com.google.android.gms.unstable. A corresponding instrumented control in
persistent, using the existing global-Java-deoptimization diagnostic but
no certificate override, also receives HTTP 400 over HTTP/2. Netlog records
cert_status=0 and a known root. Thus moving the diagnostic call from the
main process to the service's process does not supply a positive result.
A separate observer without global deoptimization crashes before an HTTP
result and is inconclusive. The native-client TLS failure and the
instrumented HTTP refusal must not be conflated.

A subsequent signal-only persistent-process diagnostic suppresses the
device-key POST after collection. The returned token decodes to 2,416
bytes and contains none of ERROR, EXCEPTION, STACK_TRACE or timeout
markers; the client's `boij.b(Context, Throwable)` error-result generator
is not called. This excludes that particular client-side exception wrapper
for this collection, but does not prove that Google's VM considers the
device trustworthy. Neither token contents nor existing key contents are
exported. A successful fresh device-key issuance is still unobserved.

[Falcon source](https://github.com/AHMED2111X/falcon_integrity_fix),
[TAKR](https://keybox.kowx712.cc/),
[inspected AlwaysStrong fork](https://github.com/BloodCorrupt/AlwaysStrongFork),
[Google published roots](https://android.googleapis.com/attestation/root).

The regression uses fresh test-only EC and RSA authorities, not those
supplied private keys. It exercises both key algorithms, PEM formats,
exact challenge and caller binding, DER authorizations, sign/verify,
certificate-chain signatures, persistence across data-app runs, rejected
XML entities/oversize challenges/StrongBox, invalid input preserving the
previous alias, explicit experimental claims and a local signing alias.
The full `tools/build-tests.sh arm` and `tools/build-tests.sh win` runs each
finish with 33 result groups and 0 failed checks, sequentially under the
shared `heavy.lock` with `MEGLER_JOBS=4`. The ARM data app passes both runs,
including persisted-chain verification; its harness reports 470 and 485 checks,
each with 0 failed. No phone has been updated with this draft and no
successful Google device-key or Play Integrity result is established.

**Following it on a phone** (flags in `Pictures\Megler\diag\<package>\flags`):
`trace-syscalls` (a library's calls by number with the path each names:
"/sys/fs/selinux/enforce" = -2 from `libdB21738DAE92C.so+0x2e18`, its own
`syscall()`), `trace-files-libdB` (the files that library opens or looks at
through the C library), `trace-calls-bywg` with 26.43.30 (`bvts` with
26.41.31: the refusal, and the answer's extra bytes), and
`trace-calls-com.google.ccc.abuse.droidguard.DroidGuard` (the machine's Java:
the flow it was made for, and `run`'s and `ss`'s results with their length).
On an older package with hardware breakpoint support, also set
`pristine-<library-name>` using the actual VM name. The fix selects
that mode by default.
Expect the loader's "left as the file has it" line with one system call
at a hardware breakpoint, zero PLT rewrites, and no warning about falling
back to a patched instruction. With `trace-syscalls`, calls from
`libdB21738DAE92C.so+0x2e18` should still reach Megler.
For 26.43.30 also use `trace-calls-byud` to locate the creation-response
parser's failure relative to those returns. Keep the sequence for the same
flow and thread: the "full" machine's return, the fast-refresh request,
then `bywg`'s construction with "byteCode" and the extra's length. A
class-loader or native failure before the return is a different result
from the qemu trace above. After a prospective fix, check that a device key
is stored and loaded on the next start, and that Google's network location
actually supplies a fix; the absence of `DGFRR` alone proves neither.

## Google's own Play services from Play Store: their splits

Play Store updates Google's own Play services as it installs any app from a
bundle: a base and splits of the phone's density and languages (26.43.30 on a
Lumia 650: "its base and 10 split(s) kept", for 320 dpi in English and
Russian). The library keeps them as an app's (`<apk>.splits`), and Google
Play services' own package runs with them. An app that hosts them had the base
alone: the shell copied `GmsCore.apk` into `<shared>\services\<token>` and
nothing else, and their resources were the base's table. A resource whose
every value is a density's -- the Google logo `googlelogo_standard_color_74x24`
of `ProductLockupView`, its default when the layout names none (in 26.41.31's
universal APK PNGs for mdpi to xxhdpi and a vector for anydpi, nothing of no
density; `0x7f0804bc` in 26.43.30) -- is in the density's split alone, and
"Manage your Google Account" (their account settings' Compose screen) and
"Help and feedback" (`HelpChimeraActivity`'s layout) ended Play Store on
`Resources$NotFoundException: Resource ID #0x7f0804bc` (0.1.0.685 and 686).
Play Store's own table has an entry under that number: the lookup was in
theirs.

Now `registerServiceApk` copies the splits beside the base
(`<token>\GmsCore.apk.splits`, `files::copyFilesWhole`: each whole, a copy
registered before them gets them at the next registration), the player passes
the folder (`megler_run_config.gms_splits_dir`) and holds its files open as it
holds the base, and the core mounts each split beside `GmsCore.apk`
(`/system/priv-app/GmsCore/split_<name>.apk`, `Native.gmsSplitPaths`; the log
says "Google Play services are in N APKs"). `megler.MicroG` makes their
`AssetManager` of the base and the splits: the splits' tables are merged into
the base's (`Resources.meglerPackageTable`, each cut to the languages as the
base's is), a file the base has not is read from the split that has it, and
their `ApplicationInfo` names the splits as Android does. A split with code
or native libraries is said in the log; nothing of either is taken from one.

And a context of theirs made for another configuration
(`createConfigurationContext`: AppCompat's `attachBaseContext2` wraps an
activity's base in one where the app's night mode or locales differ, and their
own code makes such contexts) is their resources under that configuration. It
was the app's, through the app's `ContextImpl`, which `ContextWrapper` asks.
So was their package context's theme, from which each activity of theirs
begins (`ContextThemeWrapper` copies its base's): the app's own, whose
attributes stood under their numbers in theirs wherever their theme set none.
It is a theme of their resources now, the platform's default for their target
(`Theme.DeviceDefault.Light.DarkActionBar`), and an activity of theirs that
names no theme (their application names none) has that default too, not the
number of the app's theme.

An activity of theirs whose `onCreate` throws is taken off the stack at once
and never destroyed (`AppRunner.launch`), the one below resumed; the app goes
on, as it did. Their help screen failed so and finished itself, and its
destruction at the next idle threw in their lifecycle ("State must be at
least 'CREATED' to be moved to 'DESTROYED'").

The test app `gms-host` hosts a stand-in of theirs built as Play Store's
bundle (`gms-own`: a base and the splits `config.mdpi`, `config.xxhdpi` and
`config.en`), unpacked as the shell unpacks one for 160 dpi: an activity of
theirs reads a picture of their density's split alone and a word of their
English split, and both through a context of another configuration, and finds
no attribute of the app's theme in its own (`gms-host` has a theme with one);
and one whose `onCreate` throws.

## Android's Geocoder

An app's `android.location.Geocoder` asks Google Play services' geocode
provider, as Android's location manager does: their service for
`com.android.location.service.GeocodeProvider` -- Google's own
`com.google.android.gms.location.geocode.GeocodeService` (not exported; in
26.41.31 a Chimera proxy whose `GeocodeChimeraService` gives the binder of a
`com.android.location.provider.GeocodeProvider`, its legacy provider below
Android 15, `GeocodeProviderBase` from 15 on), or microG's
`GeocodeProviderService` (OpenStreetMap's Nominatim). It is bound as the system
binds it and kept (`MicroG.bindAsSystemNow`, on the main thread), and its
binder, local, is called in place on a thread of the Geocoder's own with the
app's package, uid, attribution tag and locale (`GeocoderParams`). The
framework has the library's `GeocodeProvider`, which neither carries, the
provider's interfaces (`IGeocodeProvider`, `IGeocodeListener`) and Android's
`Address`. Where no provider is there, `isPresent()` is false and a request
has no addresses and no error, as on Android. A synchronous request asked on
the main thread binds the provider there before it waits; what the provider
wants of the main thread while it answers waits with it (a phone's provider
has a process of its own). Whether Google's server answers their geocoder
here is for a phone to show ("I/Geocoder: a place asked ... address(es)").

Windows' own `MapLocationFinder` is no way round: without a Bing Maps key
(`MapService.ServiceToken`) it answers `InvalidCredentials` to a reverse and
a forward geocode alike (tried on this PC, Windows 11 26300).

## Class isolation

The failed implementation reverted in `1adae04` appended GmsCore's DEX files
to the client's global class path. Client Google SDK classes then displaced
microG classes with the same names and different fields, notably
`GetServiceRequest.CREATOR`.

`megler.ServiceClassLoader` now assigns all DEX files in a service APK a
separate namespace. Definitions, linked classes, DEX type/member caches,
superclasses, interfaces, reflection field/method types and reference arrays
resolve in that namespace. Framework/libcore classes remain shared. Class
mirrors identify the defining loader, and GC retains the loader. JNI class
wrappers do not overwrite the client's wrappers; FindClass in a scoped native
method uses its defining namespace. This is an explicit service-loader path;
ordinary arbitrary Android ClassLoader delegation is still not fully implemented.

## A module's own scope (child scopes)

Google Play services loads each of its Chimera and Dynamite modules (Cronet,
the ads module, the Dynamite loader itself) through a class loader of its own
over the module's APK: a `DelegateLastClassLoader` once SDK_INT is 29 or more
(Android 10's four-argument constructor, which `core/libcore-overlay` adds to
Android 9's class), or a `PathClassLoader` of Google's that looks in its own
files first; its parent is the service's loader, another module's, or a filter
over them or the boot class path. Such a loader's classes, and those of any
`DelegateLastClassLoader`, get a scope of their own, from 16 on
(`mg_dvm_art.c`, child scopes): a name is looked for among the platform's
classes first, then in the loader's own files and its parent's scope, in the
order the loader asks them (its own first for a delegate-last loader, its
parent's first otherwise; a parent of the boot class path alone adds nothing).
The parent's scope is found by walking the loader's parents, and through a
filter loader of no files of its own, its fields. A child's native libraries
come from its APK in a linker scope of the same number, so the Cronet module
loads its own `libcronet.<version>.so` (stored in the APK) and not the one Play
Store embeds. A loader whose parents reach the app's own loader and that asks
its parent first (an app's plug-in) still puts its classes on the app's path;
so does every loader before the app's own is made, and a file with no DEX in it
gets no scope. Which service a child's code is of (a thread's owner, the
uncaught exceptions of a service's thread, guarded native calls) is its root
scope's (`mg_dvm_scope_root`).

Before, a module's classes went on the app's one class path, where the app's
classes of the same short obfuscated names won: Google Play services 17's ads
module called Play Store's `kk.a` (NoSuchMethodError), and 26's modules ended
Play Store 53 with "Cannot cast ruh to algy" and a NoSuchMethodError of
`j$.util.Objects`. The data app's step `delegate-last-scope` checks the order
with a DEX of its own (`module-src/`, carried as `assets/module.dex`): a
`DelegateLastClassLoader` gets the module's `Twin`, a `PathClassLoader` whose
parent is the app's gets the app's.

An attribution context of Google Play services' (`createAttributionContext`,
which version 26 makes for much of its work) is its package context itself:
`ContextWrapper` now asks its base for one, as Android's does, and the base of
the service's context is the app's, which bound the service's own services as
the app. A binding meant for Google Play services that none of its services
takes is said in the log ("no service of Google Play services binds ...").

The package context's own attribution tag (where none was asked for) is
`megler.app` only where the user lets Google Play services' own features use
the location, and null otherwise: Google's GmsCore refuses a location request
of its own whose identity (`ClientIdentity.c(Context)`, made of that tag) has a
null tag or its location module's ("internal location usage has a forbidden
attribution tag"), and those requests are its own features' in every app's
process -- personal safety, semantic location, place suggestions --, not the
app's. One answer for every app, asked over the app the first time such an
identity is made, kept in Megler's settings (docs/JAVA.md, "Permissions").
microG makes no such identity, and is never asked about.

A service of Google Play services is given one `onBind` for each intent that is
different as Android's `Intent.filterEquals` has it -- its action, data, type,
identifier and categories, not its extras (`MicroG.bindKey`) -- and the clients
of the same intent share that binder, as on Android; each new one is a line in
the log ("bound ... for ..."). They were told apart by the action alone. GmsCore
26's container services (`GmsApiService`, the bound brokers) serve every API of
the modules they hold, and the intent a client is given for an API by the
chimera provider's dynamic lookup (`BoundService.getStartIntent`) is the same
action for all of them, `com.google.android.chimera.BoundService.START`, with
the API in its data (`chimera-action:<the API's action>`): the broker made for
the first API a container was asked for was handed to the clients of all the
others, which it refused ("Unexpected service id: 343" to `GoogleAuth.API`, and
40, 41, 270... about a hundred a minute). Google Play services' own sign-in did
not connect, and WhatsApp's search for its Google Drive backup failed until it
said it took too long (package 486). The microG test checks the keys.

## Aurora compatibility inventory

This inventory uses microG v0.3.17.252432, commit
`32bc8954ff872d0e1a05ddfae81561b6dbecef69`, as a reference. Its
`settings.gradle` and `play-services-core/build.gradle` describe source modules
and build dependencies, not proof that a particular downloaded APK contains a
feature or that it works on Aurora. The manager may install a later release.
The provider remains optional and disabled by default.

| Area in the reference source | Aurora status and remaining dependency |
| --- | --- |
| Base/basement, tasks, Chimera, Conscrypt, Cronet, common broker and OSS licenses | Local service routing and Binder callbacks work with the synthetic guest fixture below. Actual APIs, dynamic modules and native libraries require separate tests with the installed microG APK. Cross-process Android Binder is absent. |
| Advertising/identifier, ads APIs, app set, app invite | Source modules exist; no Aurora API result has been verified. Advertising and app-set identifiers must not be invented by the bridge. |
| Account authentication, sign-in, Firebase auth, credentials, blockstore, work account and FIDO | The user reported completing microG sign-in on an Aurora tablet. That does not establish app token access, account visibility, passkeys or per-app consent. These require real-device, permission and upstream verification; no account or token is used in the fixture. |
| Check-in, GServices, phenotype, clearcut, measurement, Firebase dynamic links, compliance | Source and provider/receiver routing exist; these functions and their network or persisted state are not confirmed by the synthetic fixture. Server-dependent behavior depends on upstream services. |
| GCM/FCM and instance ID | Source includes a persistent MCS network connection. Delivery when a client is stopped needs an authenticated shared service and lifecycle support; the current per-client service instances do not supply this. Registration and message delivery are not verified on Aurora. |
| Location, places, place reports, maps and panorama | Source modules exist; location, map rendering and geocoding are not verified on Aurora. Do not feed real location or scan data to microG without reliable per-app authorization and user consent; maps and places can also require external providers or network access. |
| Cast, Nearby, wearable, Thread networking, fitness, vision and ML Kit barcode/face detection | Source modules exist; actual availability varies by build flavor, upstream implementation, hardware, permissions and native libraries. No Aurora end-to-end support is established. |
| Drive, games, payments/tap-and-pay, identity credentials and Google account features | Modules or client APIs exist, but service implementations, backend authorization and device capabilities vary. No Aurora end-to-end support is established. |
| reCAPTCHA, SafetyNet and DroidGuard | Source modules exist, but trusted attestation or Play Integrity verdicts cannot be supplied by a compatibility shim. They are outside this work and are not emulated. |

For cross-process functions, a future service boundary must authenticate the
actual OS caller and bind it to a verified installed app and its launch
session, authorize each requested operation and permission, and account for
service death, revocation and delivery to stopped apps. A caller-supplied
package name, a shared socket alone, or the manager's provider-status D-Bus
method is not that boundary. Its IPC and ABI are not implemented here.

The build-only `ANDROID_COMPATIBILITY_LAYER_BUILD_SHARED_MICROG_PROBE` fixture
is off by default and is not installed. Its fixed, typed test response uses
the actual D-Bus sender, but does not identify or authorize the caller. Run
`aurora/tests/shared_microg_headless/test.py` with the fixture and client
paths to test separately started and D-Bus-activated processes on private
local buses. The fixture impersonates the manager name on that isolated bus;
never run it on the device session bus. This does not test the installed
manager, Aurora activation, an exported Android standalone, or microG.
The installed manager's existing `GetProvider` and `InspectCaller` are
separate diagnostic methods; the native `dbus-identity` client can check them.
Aurora's single application `ExecDBus` already launches the manager, and a
separately activated executable in the same RPM is not yet established.
On an ARM64 tablet, the native `dbus-identity` client observed no manager
name owner before `GetProvider`, then received `microg` without a manager
window. This does not verify a generated Android standalone or a Google API.

Generated standalones declare the manager's main interface, not its `microg`
interface. The optional `ANDROID_COMPATIBILITY_LAYER_STANDALONE_MICROG_DIAGNOSTIC`
build enables a no-argument `DiagnosticPing` on that main interface and a
diagnostic screen in newly exported standalones. The manager records each
successfully exported RPM's app ID and Android package in its private registry,
including on cache reuse. It looks up the executable identified for the actual
D-Bus sender, and returns only `synthetic-ok` for known IDs. Unknown or
unidentified callers are refused; the client cannot supply an ID. The option
is off by default and changes neither microG nor its login.

The registry records what the manager exported, not installation, APK integrity,
or authorization to use an account or Google API. `IdentifyByBusName` provides
an executable and key, but standalones share launcher code and a test signing
key. There is still no trusted installed app ID-to-APK attestation or
cross-process Binder boundary. The standalone screen reports owner-before-call
and ping separately; neither a successful ping nor the native client test
demonstrates a Google service. On an ARM64 tablet, the user reported
`Owner before request: absent` and `DiagnosticPing: synthetic-ok` from the
installed standalone launcher. This confirms a response and a sender match in
the export registry after no owner was observed. The user also confirmed that
the manager window did not appear, demonstrating cold activation without a
window in this run. Google APIs and a shared GmsCore were not tested.
The user subsequently reported that the selected Android app crashed after
Continue, followed by manager crashes on later normal launches; after a reboot,
the manager opened when that standalone had not been launched. The user also
reported crashes with previously working APKs. A separate diagnostic-page
component lifetime defect has since been corrected and tested locally, but its
connection to either device crash is unconfirmed. Both failures remain under
investigation; the overall device lifecycle test is not complete.
In a later cold run without Continue, the manager crashed when opened while
the standalone diagnostic screen was still visible, so neither Continue nor a
guest crash is required for the manager failure. A run with the manager
started under the Aurora IDE C++ debugger with `--microg-service` (QML
debugging disabled, because its extra argument makes the service mode exit
with code 2) showed only a blank splash; a manual interrupt showed an idle Qt
event loop, not a fatal signal, and the UI appeared once the debugging session
ended. That behaviour is attributed to the debugger session and is not
evidence about the cold-device crash, whose cause is still unknown.
The code shows a common path independent of microG: standalone and preview
processes start the log client (in the diagnostic build only after Continue;
before that, `DiagnosticPing` itself starts the manager), and the client calls
`StartServiceByName` for the manager when no owner exists. Unless activation
is denied or forwarding is unavailable, a standalone launch therefore starts a
windowless `--microg-service` manager that `setKeepAlive(true)` keeps until
reboot. Aurora documents that `ExecDBus` runs under
`invoker ... --id=<app> --single-instance`. In upstream mapplauncherd, a
second single-instance launch whose lock is already held only asks lipstick
to `raiseProcessWindow` and does not start the binary. If Aurora behaves the
same, tapping the icon would deliver no `activate` request to the windowless
process and leave only the launcher splash, which may look like a crash. This
matches the IDE observation but remains unconfirmed on the device; a PID check
before and after the tap alone cannot exclude a short-lived new process.
Upstream mapplauncherd sources confirm the lock mechanics: the booster takes an
fcntl lock on `$XDG_RUNTIME_DIR/single-instance-locks/<binary>/instance.lock`
inside the process that becomes the application and keeps it until that
process exits; the lock is keyed by the binary, so icon launches and
`ExecDBus` activation share it. The manager therefore adopts that inherited
lock and holds it only while its window is shown: a `--microg-service`
manager releases it at startup, and the window's `visibleChanged` drives the
lock, so every hide releases it and every show, including a later re-show of
the same window, reacquires it. Reacquiring uses the upstream range
(`F_WRLCK`, `SEEK_SET`, start 0, length 1) and retries every 500 ms for at
most 60 s while a forwarding launch still holds it. The adopted and
reacquired descriptors are close-on-exec; upstream leaves the booster's fd
inheritable, which would keep the lock file open in `adb` or guest children.
The journal gets at most one line per episode: `Launcher single-instance lock
adopted`, `No inherited single-instance lock` (IDE or non-launcher start), a
warning if a lock descriptor exists but is not owned by this process,
`Single-instance lock busy, retrying`, `reacquired after N retries`, or the
warning `not reacquired after N retries`. After exhausted retries a launch
still reaches the manager through a forwarding process; the next show retries
again. With the lock free, a tap starts a normal manager process that
forwards `activate` over D-Bus to the running manager, which shows its window.
The D-Bus-started manager now also sets `AA_ShareOpenGLContexts`, because it
can show the same UI, including the WebView report form. The lock handover is
covered by `manager_lifecycle_test` with real cross-process fcntl locks,
including hide -> show, retry exhaustion and recovery, and the close-on-exec
flag. Those checks ran on the x86_64 build host; the ARM64 binary was only
compiled, not run. The sysroot's `libapplauncherd` exports the same
`SingleInstance`/`activateExistingInstance` plugin interface and the
`invoker --single-instance` option, but the device plugin's lock path and
range were not inspected on the tablet. Whether
Aurora's mapplauncherd fork behaves exactly like upstream, and therefore
whether this resolves the device failure, still needs one tablet check. A
crash of the standalone itself after Continue is a separate issue.

A tablet run of that build still showed a long splash that then vanished. The
Aurora 5.2 home-screen sources in the SDK sysroot point to a path the lock never
reaches. RuntimeManager reports `ApplicationAlreadyRunning`, and
`RuntimeManagerIntegration.qml` exposes `launchProcess(appId)` for
RuntimeManager/mapplauncherd to bring a running application forward.
`compositor.qml` ignores `Started` with `StartReason` 6 (D-Bus) and the
`file:///no-gui` URI. `ToplevelManager.activateToplevel(null, appId)` adds a
splash-only toplevel when the app has no window. The splash's
`StartupWatcher` then calls `windowLowered` on `startupFailed`. The `.desktop`
guide says a second icon tap opens the window of the running single instance. A
manager started by `ExecDBus` has no window, and no process is started. The
vanishing splash would then be a startup timeout rather than a crash. This
matches the older report that the manager "crashed" after any standalone crash,
since the standalone log channel also D-Bus-activates the manager. This is
inferred from system sources and was not observed on the tablet. Running
`pgrep -af ru.androidcompatibilitylayer.appmanager` right after the splash
vanishes distinguishes the cases without privileges: the same
`--microg-service` PID still alive means activation without a window, while no
process means an exit or a crash.

On the tablet the `--microg-service` process was still alive after the splash
vanished, and killing it let the icon open the manager normally. The
RuntimeManager client API documents the fix path: when an application
explicitly registers `Start`, a `Start` call reaches it over D-Bus (an `invoker`
spawn plus `Start`), in the same way a running instance handles `OpenURI`. The
manager now declares `Intents=OpenURI;Start` and registers a `Start` handler
that replies at once and then shows (or raises) the manager window. The
`manager_lifecycle_test` sends `HandleIntent("Start")` to a window-less
background manager and expects exactly one window. Whether RuntimeManager on
the tablet routes the icon tap to that handler for an instance started by
`ExecDBus` still needs one device check.

A tablet run of the build with the `Start` handler still showed the splash
hang and vanish, so it did not fix the failure; whether the handler was called
is not known. The 5.2 home sources show why it may never be reached: an icon
tap emits `requestStart`, `RuntimeManagerIntegration.launch` invokes `Start`
with `preferredHandler`, and `local.Lipstick.WindowModel.launchProcess`, which
RuntimeManager and mapplauncherd use to bring a running application forward,
only calls `activateToplevel` in the compositor and sends nothing to the
application. A window-less instance cannot answer that, so every fix that
keeps a window-less application instance alive leaves the icon broken while
it runs. The remaining options are: let the window-less manager exit when it
has no clients (no visible change, but the icon still fails while a standalone
keeps it running), declare `X-Nemo-Single-Instance=no` so that a tap starts a
new instance that forwards `activate` (the home screen supports `.instance_`
app ids and an undocumented `X-Aurora-Single-Cover`; this changes how launches
are handled and is unverified), or move the service out of the application
instance.

The manager now uses the second option: its desktop entry declares
`X-Nemo-Single-Instance=no` and `X-Aurora-Single-Cover=yes`. Each icon tap
starts a new process; when the manager service is already registered, that
process calls `activate` on the running manager (window-less or not) and exits,
and the running manager shows or raises its window. With the single-cover key
the 5.2 compositor identifies the manager window by the application, not by
the launched instance, so the switcher keeps one manager entry. The key is
undocumented and only verified in the home-screen QML sources; if it is
ignored, a second cover or entry may briefly appear. Every tap now costs one
short process start, and the invoker single-instance lock is no longer used
for icon launches (`SingleInstanceLock` stays as a harmless fallback). On the
host, `manager_lifecycle_test --desktop` checks both keys and
`--actual-manager-transition` shows the forwarding launch exiting 0 after the
window-less manager accepted `activate`; the manager window itself cannot be
created on the host `minimal` platform (it crashes there even without a
background manager), so the visible result needed one tablet run: launch
a standalone (Owner absent, `synthetic-ok`), then tap the manager icon. Tablet
observation after this change: launching a standalone starts the background
process; tapping the manager icon while it runs opens the manager window; after
the manager window is closed, the background process is no longer listed. The
manager has no exit path on window close (`setQuitOnLastWindowClosed(false)`,
keep-alive, no `Qt.quit` in the manager QML), and the earlier observation that a
normally launched manager also left no process after its window was closed
points to the system closing the application process; the signal or reason was
not recorded. Later standalone requests re-activate the service over D-Bus
(provider requests auto-start, the log client calls `StartServiceByName`), but
requests in flight at that moment fail. The number of covers or switcher
entries and other launch scenarios were not checked.

For a tablet test, add
`--extra-options " -DANDROID_COMPATIBILITY_LAYER_STANDALONE_MICROG_DIAGNOSTIC:BOOL=ON"`
to the Aurora IDE `apptool build` step for the ARM64 manager and rebuild/deploy
it. The leading space inside the quotes is required by apptool. Import an
offline test APK, export and install a fresh standalone from this manager, then
restart the tablet without opening the manager. Closing its window or waiting
60 seconds does not guarantee that the background process has exited. Launch the installed
standalone before opening the manager again. The diagnostic screen should show
`Owner before request: absent` and `DiagnosticPing: synthetic-ok`; the manager
window must not appear. `present` means the run was not cold. An error is a
failed test, not an absent owner; `AccessDenied` means the sender could not be
matched to the export registry. Continue dismisses the diagnostic and starts
the Android app. This test uses no account or network.

After testing, configure the same IDE step once with `BOOL=OFF`, rebuild and
deploy, then remove the extra option. Merely removing it can leave `ON` in the
CMake cache. A standalone exported by the diagnostic build retains its
diagnostic launcher until it is recreated and reinstalled from an OFF build.

The local service router checks whether a service is exported, but does not
enforce its declared `android:permission`; the current permission model does
not reliably distinguish signature/system permissions from ordinary ones.
It also does not match manifest `<data>` constraints when selecting a service
or receiver. Consequently the synthetic test's hidden-service rejection is
not evidence of complete component authorization or intent matching. These
are blockers for safely exposing permission-protected and data-filtered APIs.

The local synthetic guest test (`core/tests/microg-guest-broker/run.ps1`)
builds the current framework and two test APKs in a new output directory. With
an existing x86_64 `mg_tests` runner and Android root, it runs the guest with
networking disabled and requires its PASS marker. It exercises service
resolution, rejection of a hidden service, a nested Parcel carrying a callback,
intent-specific bind/unbind/rebind, and a retained system binding. It tests a
stand-in, not the real microG APK, device permissions or push. The separate
`core/tests/microg-binder-boundary/run.ps1` tests local Parcel/Binder behavior
and confirms that ordinary Parcel marshalling does not transport Binder
references between host JVMs.
Neither test proves a feature works on an Aurora device.

The framework overlay has an experimental, opt-in `BinderBridge` for private
duplex streams. Its bounded wire encoding carries primitive Parcel bytes,
Binder handles and nested Bundles containing strings, integers, longs,
booleans, byte arrays, Binders and Bundles. It also serializes standalone
Parcelables through `writeToParcel` and recreates them with the reader's
`CREATOR`; their nested Parcel data is subject to the same limits and type
restrictions. Arrays of Parcelables, including null elements, are supported.
Parcelables inside Bundles, file descriptors, lists and other arrays of objects
are not supported. The transport accepts only zero Parcelable write flags and
rejects nonzero flags instead of discarding them.
Two host JVMs exercise a synchronous transaction, a nested callback, a
Bundle, a Parcelable containing a Bundle, and returning the callback to its
owner. The Bundle test requires
compiled framework classes and the boundary test's `-FrameworkClasses`
option. The bridge can apply a UID and PID supplied by its connection owner,
but does not authenticate peers or establish their identity. It has no handle
release, per-object death, file
descriptors or arbitrary Parcelables. Handles are kept until the
connection closes. Do not use it for accounts or other real data until those
gaps and Aurora integration are addressed.

`FLAG_ONEWAY` sends without waiting for a response, and the receiver invokes
the target with a null reply. Calls to the same target are dispatched in
arrival order; other targets and synchronous calls can run concurrently.
Successful sending does not guarantee delivery or execution, and receiver
exceptions cannot be returned to the caller. Both peers must support one-way
frames. Each bridge bounds outstanding one-way calls to 128 and 4 MiB of
serialized payloads, with eight dispatch workers and 32 queued target workers.
Exceeding a limit or failing to schedule a target closes the connection rather
than silently dropping a request; closing can discard queued calls. Host tests
cover asynchronous return, ordering, independent targets, receiver exceptions,
and count and byte limits.

Remote proxies also support `linkToDeath` and `unlinkToDeath` for connection
loss. Closing the bridge or losing the peer marks all proxies dead and sends
one notification per remaining registration, asynchronously outside the
bridge lock. Registrations are limited to 4096 per bridge; unlinking before
closure prevents that registration's notification, while unlinking after
closure returns false. Callbacks run sequentially on one daemon thread, so a
blocked callback delays later callbacks. A new connection creates new proxies;
automatic reconnection and per-object death independent of the connection are
not implemented. Host tests cover unlink, duplicate registration, peer death,
explicit close, callback failure, registration races and a fresh connection.
`RemoteCallbackList` removes a dead proxy and ignores a stale death notification
after a callback has been replaced or unregistered.

For an owned socket transport, provide the `shutdownTransport` constructor
callback and have it call `shutdown(SHUT_RDWR)` before the bridge closes its
streams. Closing a descriptor alone need not interrupt a read already in
progress, so without shutdown the peer may not observe disconnection. Shutdown
affects all duplicates of that socket and is appropriate only when the bridge
owns the entire connection. The three-argument constructor provides no such
socket shutdown. Callback errors are logged; callers must not assume a failed
shutdown interrupted the peer. This is a transport responsibility, not a
change to general guest descriptor or duplicate-descriptor semantics.

The Aurora-side transport groundwork can create a Unix stream socket pair and
transfer one end as a D-Bus file descriptor. The receiving helper accepts only
connected Unix stream sockets and sets close-on-exec. An isolated D-Bus
fixture passes a descriptor to a separate Linux JVM; two JVMs use that socket
with `BinderBridge` for a synthetic transaction and nested callback. The
fixture also checks that a pending call fails when the peer exits. The
optional `-LinuxJre` argument to `core/tests/microg-binder-boundary/run.ps1`
runs this fixture in the Aurora build-tools Docker image. The test inherits
host descriptors and does not run Megler's guest runtime. Guest descriptors
are managed by Megler's own descriptor table, so a host socket number cannot
simply be used as a guest descriptor. On POSIX, `mg_net_import_unix_stream`
validates and duplicates a connected host Unix stream into that table; the
caller retains the host descriptor. The guest socket owns the duplicate and
uses the existing host socket readiness and shutdown machinery. The network
tests cover invalid descriptors, transfer, wakeup, shutdown, EOF and ownership.
`tools/shared-microg-guest-fd-test.sh` also exercises D-Bus handoff into that
table with a synthetic byte exchange, not Java or BinderBridge.
Run it from the repository root on Linux with Qt 5 D-Bus development files,
GCC and `dbus-run-session`, setting `WORK_DIR` to an empty output directory.

`core/tests/microg-binder-boundary/guest.ps1` builds an x86_64 Linux runner
and a synthetic landscape Android Activity. The isolated D-Bus fixture passes
its socket to the runner, where a test-only native imports it into the guest
descriptor table. The guest uses `ParcelFileDescriptor` and `BinderBridge` to
exchange a transaction, nested callback and one-way callback with a host JVM,
then verify the death notification when the host process exits. It also runs
two separate guest runtimes, checks both PASS markers, and verifies that the
server's explicit socket shutdown delivers a death notification to the client.
Another two-guest run binds a synthetic GMS broker APK through the server's
normal service router, sends its Binder handle to the client and exercises a
broker transaction with a nested callback. With `-MicrogApk`, an additional
isolated run binds real microG's advertising ID service and common service
broker, including a request containing a `Feature` array and a nested callback.
The experimental `SharedMicrogServiceRouter` binds requested GMS actions or
components rather than selecting pre-bound services, and releases a connection's
bindings when closed. Its synchronous protocol currently carries only an action
and component: data, types, categories and extras are not implemented. Caller
identity is an optional connection property, not part of this protocol. The
router reports an unavailable service as a failed bind rather than a transport
exception; actual transport and protocol failures remain errors. The client
releases the server's binding record even when no service was returned. This
router also handles production shared-service binds.
The fixture checks process exits and disconnect, without network or account
access. It requires JDK 8, Android SDK 29, an x86_64 Android root, a Linux JRE,
the local Aurora build-tools Docker image and framework classes compiled from
the current checkout, supplied via `-FrameworkClasses`. Output is written to
the new directory given by `-Output`; `guest-binder.log`, `two-guests.log` and
`broker-guests.log` contain the runs; the optional real microG run writes
`real-guests.log`. The real run requires the SQLite amalgamation in
`build/sqlite` and the build image's system SQLite development files for its
WAL fixture. It checks guest `/tmp` paths, SQL `ATTACH`, in-memory and read-only
databases, and recovery of a committed row from a WAL created by system SQLite.
The guest VFS converts guest paths once; the existing native VFS remains the
default. These isolated runs do not test an Aurora device, accounts or network.

The manager's shared microG backend is opt-in: it is configured only when the
provider setting is `microg`, the supported APK and runtime are available, and
the app/data directories can be prepared. It launches the current manager
executable in its early `--shared-microg-worker` mode, keeps worker data in a
separate persistent directory, and returns an `OpenChannel` descriptor only
after the worker has sent `READY` and acknowledged `OPEN`. Startup, resolver,
or worker failures leave the channel unavailable and close the local stream.

For caller mapping, the manager obtains the actual D-Bus sender's executable
and signing-key ID from Aurora's
[identification service](https://developer.auroraos.ru/doc/5.2.1/software_development/reference/security_and_management/identification_service)
and matches the executable's exported app ID to the manager's export registry;
caller-supplied identity fields are not accepted. The key ID is required to be
present but is not checked against an installed-package signing policy. The
registry records an export, not proof of installation or identity, so this is
a limited manager-derived mapping, not APK attestation or full authorization.
The isolated original-APK test covers the self-spawn dispatch and
manager-to-supervisor-to-worker handshake, but does not verify Sailjail access,
the device's identification service, the guest client-router startup wiring,
accounts, or network behavior.

For the guest test on Windows, with an existing Linux x86_64 runner, Android
root, JDK 8 and Android SDK 29, and the local `aurora-build-tools:5.2.1.200`
Docker image:

```powershell
.\core\tests\microg-guest-broker\run.ps1 -Jdk C:\path\to\jdk8 `
  -AndroidSdk C:\path\to\sdk -AndroidRoot C:\path\to\root-x86_64 `
  -Runner C:\path\to\mg_tests -Output C:\path\to\new-test-output
```

The script requires a new output directory, compiles the framework there,
and does not pull an image or use a network. Its `guest.log` contains the
runtime output. A prebuilt matching framework can instead be supplied with
`-FrameworkDexDirectory`.

## Verification

`tools/microg-fetch.sh` downloads upstream microG `v0.3.17.252432` and verifies
its pinned SHA-256. No build downloads it automatically.

With the normal Android-root dependencies present, `tools/build-tests.sh arm`
also runs the `microg` test when `build/microg/GmsCore.apk` exists. The client
fixture intentionally defines an incompatible `GetServiceRequest.CREATOR`.
It checks independent class identities, reflection and arrays, shared Binder
identity, resources/storage isolation, Advertising ID transactions,
unbind/rebind, native `libconscrypt_gmscore_jni.so` loading in the service ELF scope, and the actual `IGmsServiceBroker.getService` transaction 46
with a callback returning `ICommonService`.

A focused run of the built ARM runner is:

```sh
MG_JAVA_GMS_APK=/path/to/GmsCore.apk qemu-arm -L /usr/arm-linux-gnueabihf   /path/to/mg_tests --java microg /path/to/microg.apk /tmp/microg-data   /path/to/android-root -
```

The `microg` runner requires the PASS marker and fails on missing callbacks
or a reported fixture exception; an ordinary `--java app` exit code alone
is not evidence that a real application started successfully.
