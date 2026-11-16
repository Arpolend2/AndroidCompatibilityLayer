# DroidGuard and Google service compatibility progress

Status: 17 November 2026. This is an engineering progress report, not release notes.
No release, package deployment or build-number update is part of this integration.

## What the original failure meant

Google Play services runs inside each Android client's Megler process. The original
`DGFRR` exception mentioning `byteCode` occurred while interpreting a signed
DroidGuard creation response that did not contain a program. It did not by itself
identify a crash in the downloaded native VM. Other runs exposed distinct VM,
timing and network failures, which needed separate controls.

The device-key request goes to `https://android.googleapis.com/auth/devicekey`.
GMS tries to parse the streaming response as protobuf before reporting its HTTP
status. An HTML HTTP 400 response starts with a byte that looks like a protobuf
end-group tag, explaining the misleading `Parsing Exception` observed on the
phones. The later request/status traces confirmed the actual HTTP refusal.

## Runtime fixes already integrated

- Preserve the original instructions of DroidGuard's ARM VM, including when
  Google rotates the downloaded library. Megler's interception must not change
  the code the VM inspects.
- Refuse connections to closed local TCP ports promptly on Windows ARM. A VM
  loopback probe previously consumed roughly 130 seconds, exceeding GMS's
  60-second collection deadline. Removing that delay lets collection finish.
- Report the Android keystore cause for unsupported attestation, including
  error -100, rather than losing it in a generic provider exception.
- Expose hosted GMS's own manifest permissions and retain the existing service
  and class-loader compatibility work.

These fixes improve actual execution. They do not establish that Google accepts
the resulting device signals or issues a device key.

## Experimental features integrated in this batch

All three mechanisms below remain opt-in. No keybox, private signing key,
automatic keybox downloader or fixed device profile is shipped with them.

**PlayIntegrityFork-style profiles.** A client's data directory can hold
`custom.pif.prop`, `custom.pif.json`, `pif.prop` or `pif.json`. The implementation
supports Build fields, explicit native/Java property names, and DroidGuard-specific
certificate-chain blocking. It validates unsupported options and profile values.
Signature spoofing and Play Store-specific spoof modes are not implemented;
wildcard properties must be expanded into explicit names. Because hosted GMS
shares the client's process, the configured Build/property changes affect that
whole process. Without a profile, they are inactive.

**Play Integrity Break-style error handling.** The `play-integrity-break` flag
enables synthetic error callbacks for applications that can take an alternate
path after an Integrity API failure. It does not manufacture a Google integrity
token. In the HP Claude comparison, the visible error changed from a Play update
requirement to an app-verification failure; successful sign-in was not obtained.

**External-keybox attestation.** The keystore can issue EC/RSA attestation leaves
using an explicitly supplied issuer in the client's `attestation` directory.
The feature requires `attestation/enabled` and external configuration/key material.
It validates signer/key agreement, certificate signatures and validity, CA
constraints and bounded input, and rejects XML entities. Leaves bind the generated
public key, requested challenge and application identity, and their chains persist
with the key. Invalid input preserves an existing alias.

Keys remain software-held. Software is the default attestation security level;
explicit experimental TEE claims do not provide actual hardware isolation or
verified boot. Unsupported StrongBox, authentication, unique-ID and device-ID
authorizations are rejected. The separate StrongBox fix preserves a caller's
requested setting so that it can be handled correctly instead of silently lost.

When combining a profile with external-keybox testing, set `spoofProvider=0`:
otherwise the profile's DroidGuard-specific chain blocking prevents DroidGuard
from consuming that attestation chain.

## How the supplied Android modules relate to Megler

The Magisk/Zygisk/LSPosed packages cannot simply be installed into Megler:
their boot, process-injection and Android-keystore services differ from this
runtime. The integration implements the relevant profile, callback and keystore
behaviour directly. It does not add Magisk, Zygisk, LSPosed or an Android kernel.
SUSFS-style kernel hiding is not implemented by this work.

TrickyStore supplies the reference for external-keybox chain handling, but a
well-formed chain is only one input to Google's checks. The audited official
PlayIntegrityFork v19 archive contains no keybox. Falcon and AlwaysStrong-style
refreshers distribute or replace external key material; they cannot generate
Google trust or guarantee that a downloaded key remains accepted. Their updater
scripts are not incorporated into Megler. The inspected TAKR 41 entry is a
catalogue identifier, not an engine version, with an observed certificate expiry
on 25 November 2026. These are dated audit findings, not a current endorsement
of a public keybox or download service.

## What the controls established

- ARM/qemu reproduces the GMS request independently of the phones. Captures show
  nonzero device/session identifiers, matching body/header device identifiers,
  version metadata and a nonempty DroidGuard result. Check-in can succeed while
  device-key issuance still fails.
- Both inspected Google GMS builds themselves use `missing_token` in request
  field 6. A microG-source comparison identifies the field as a GCM token. On
  Mi 4c, standard GCM registration succeeds, but substituting that token into a
  diagnostic device-key request still returns HTTP 400. This is not a fix.
- The audited PIF47 keybox certificates were listed as compromised. A separately
  audited Falcon sample had a verifiable chain to a published Google root and
  was not on the downloaded revocation list on 15 November. That local result
  did not establish server acceptance. Its observed certificate expiry is
  25 November 2026; this report is not a current recommendation to use it.
- Temporary, separately approved PIF/TrickyStore controls on rooted Mi 4c produce
  locally verifiable attestation chains but do not establish fresh device-key
  issuance. The original phone key and account data are preserved.
- Google Translate was signature-checked and installed as a Google-signed
  external client. It can translate successfully after failing to obtain Spatula:
  its interceptor times out and continues without the header. Translation success
  therefore cannot serve as proof of device-key issuance.
- An approved native-client control temporarily renames the old key and restarts
  GMS, with no Frida attachment to GMS or the VM. PIF's property hook and Build
  changes and the temporary TrickyStore process are observed. Fast refresh and
  AppCert fail with `ERR_CERT_INVALID`; no new key appears within 100 seconds.
- AppCert actually runs in `com.google.android.gms.persistent` on that Android
  reference, while DroidGuard runs in `.unstable`. A separate instrumented
  persistent-process control reaches HTTP/2 with certificate status 0 and a
  known root, then receives HTTP 400. This must be distinguished from the native
  client's earlier TLS failure.
- A signal-only control finds no plaintext error/exception/timeout markers in
  the decoded result and does not invoke the client's exception-result generator.
  It suppresses the subsequent device-key POST. This excludes one error-wrapper
  explanation; it does not decode the VM's opaque verdict or prove trustworthiness.

After the native-client controls, the original key is restored and compared with
its on-phone backup. All session-created PIF/TrickyStore directories are removed
and the phone rebooted. LG Wing is excluded from testing. Private requests,
tokens, keys, account details, locations and Wi-Fi names are not committed.

## Related integration work preserved

This integration also retains Claude's subsequent MicroG RE support, class-loader
validation for cached layout constructors, and Cronet diagnostics from integration
commit `194ef7c8`. The `cronet-netlog` and `no-quic` flags provide additional tools
for future comparisons. Netlogs and captured bodies can contain credentials and
must be treated as private diagnostic artifacts.

## Validation and remaining work

The combined framework and Java test fixtures were rebuilt before testing.
`tools/build-tests.sh arm` and `tools/build-tests.sh win` both exit with status 0:
33 result groups each, every group reporting 0 failed. They run sequentially under
the shared `build/heavy.lock`, with `MEGLER_JOBS=4` and JVM processor/memory limits.

The ARM data-app runs explicitly cover profile loading and rollback, synthetic
Integrity callbacks, the StrongBox specification and external-keybox attestation.
Both data-app runs pass (470 and 485 checks, 0 failed), including persisted chains.
These are local compatibility checks, not successful Google server attestations.

No successful fresh Google device-key issuance, positive Google Play Integrity
verdict, Claude sign-in, or Google Wi-Fi positioning fix attributable to a newly
issued key has been demonstrated. A valid local certificate chain, nonempty VM
output, successful GMS check-in or disappearance of a UI warning is insufficient
to claim those outcomes.

The next useful reference is a Google-certified Android installation observed
in its stock state before modifying its bootloader or installing root. Without
root, observing a fresh private GMS key or the decrypted exchange is not guaranteed.
The outstanding task is to obtain a demonstrably accepted exchange and compare
its request and signal collection against Megler, while keeping genuine TLS and
server refusals separate.

Detailed chronology, configuration and evidence are in [MICROG.md](MICROG.md).
