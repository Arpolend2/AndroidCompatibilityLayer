# Run with -Jdk <JDK directory> -AndroidJar <platform android.jar> -Output <scratch directory>.
# Optional -LinuxJre runs the isolated Linux D-Bus descriptor/Binder integration test in Docker.
# This tests the real Megler overlay in two host JVMs, not Android or Aurora IPC.
# The expected missing cross-process objects must change when a real bridge is implemented.
param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$AndroidJar,
    [Parameter(Mandatory = $true)][string]$Output,
    [string]$FrameworkClasses,
    [string]$LinuxJre,
    [string]$DockerImage = 'aurora-build-tools:5.2.1.200'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
New-Item -ItemType Directory -Force $Output | Out-Null
$sources = @(
    'core\framework-overlay\android\os\Parcel.java',
    'core\framework-overlay\android\os\RemoteCallbackList.java',
    'core\framework\src\android\os\Binder.java',
    'core\framework\src\android\os\IBinder.java',
    'core\framework\src\android\os\IInterface.java',
    'core\framework\src\android\os\RemoteException.java',
    'core\framework\src\android\os\ServiceSpecificException.java',
    'core\framework\src\android\util\AndroidException.java',
    'core\framework-overlay\megler\BinderBridge.java',
    'core\framework-overlay\megler\Fill.java'
) | ForEach-Object { Join-Path $root $_ }
$sources += Join-Path $PSScriptRoot 'BinderBoundaryTest.java'
$sources += Join-Path $PSScriptRoot 'BinderBridgeTest.java'
$sources += Join-Path $PSScriptRoot 'BinderCallerTest.java'
$sources += Join-Path $PSScriptRoot 'android\os\Process.java'
& (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp $AndroidJar -d $Output $sources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp "$Output;$AndroidJar" BinderBoundaryTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp "$Output;$AndroidJar" BinderBridgeTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp "$Output;$AndroidJar" BinderCallerTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($FrameworkClasses) {
    & (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp "$Output;$FrameworkClasses;$AndroidJar" -d $Output (Join-Path $PSScriptRoot 'BinderBundleBridgeTest.java')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\java.exe') -cp "$Output;$FrameworkClasses;$AndroidJar" BinderBundleBridgeTest
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp "$Output;$FrameworkClasses;$AndroidJar" -d $Output `
        (Join-Path $root 'core\framework-overlay\android\accounts\IAccountAuthenticatorResponse.java') `
        (Join-Path $root 'core\framework-overlay\android\accounts\AccountAuthenticatorResponse.java') `
        (Join-Path $PSScriptRoot 'AccountAuthenticatorResponseBridgeTest.java')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\java.exe') -cp "$Output;$FrameworkClasses;$AndroidJar" AccountAuthenticatorResponseBridgeTest
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp "$Output;$FrameworkClasses;$AndroidJar" -d $Output `
        (Join-Path $root 'core\framework-overlay\android\accounts\AbstractAccountAuthenticator.java') `
        (Join-Path $PSScriptRoot 'AccountAuthenticatorTransportTest.java')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\java.exe') -cp "$Output;$FrameworkClasses;$AndroidJar" AccountAuthenticatorTransportTest
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp "$Output;$FrameworkClasses;$AndroidJar" -d $Output `
        (Join-Path $PSScriptRoot 'AccountAuthenticatorIntentBridgeTest.java')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\java.exe') -cp "$Output;$FrameworkClasses;$AndroidJar" AccountAuthenticatorIntentBridgeTest
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp "$Output;$FrameworkClasses;$AndroidJar" -d $Output `
        (Join-Path $root 'core\framework-overlay\megler\SharedMicrogAccounts.java') `
        (Join-Path $PSScriptRoot 'SharedMicrogAccountsTest.java')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\java.exe') -cp "$Output;$FrameworkClasses;$AndroidJar" megler.SharedMicrogAccountsTest
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $identityOutput = Join-Path $Output 'identity'
    New-Item -ItemType Directory -Force $identityOutput | Out-Null
    & (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp "$FrameworkClasses;$AndroidJar" -d $identityOutput `
        (Join-Path $root 'core\framework-overlay\megler\SharedMicrogIdentity.java') `
        (Join-Path $root 'core\framework-overlay\megler\PhoneApps.java') `
        (Join-Path $root 'core\framework-overlay\android\content\pm\PackageManager.java')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp "$identityOutput;$FrameworkClasses;$AndroidJar" -d $identityOutput `
        (Join-Path $PSScriptRoot 'android\os\Process.java') `
        (Join-Path $PSScriptRoot 'identity\megler\Native.java') `
        (Join-Path $PSScriptRoot 'identity\megler\SharedMicrogIdentityTest.java')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\java.exe') -cp "$identityOutput;$FrameworkClasses;$AndroidJar" megler.SharedMicrogIdentityTest
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp "$identityOutput;$FrameworkClasses;$AndroidJar" -d $identityOutput `
        (Join-Path $root 'core\framework\stubs\dalvik\system\VMStack.java') `
        (Join-Path $root 'core\framework-overlay\android\os\Process.java') `
        (Join-Path $PSScriptRoot 'identity\megler\SharedMicrogClientIdentityTest.java')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Jdk 'bin\java.exe') -cp "$identityOutput;$FrameworkClasses;$AndroidJar" megler.SharedMicrogClientIdentityTest
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if ($LinuxJre) {
    $LinuxJre = (Resolve-Path $LinuxJre).Path
    $Output = (Resolve-Path $Output).Path
    $AndroidJar = (Resolve-Path $AndroidJar).Path
    if (!(Test-Path -LiteralPath (Join-Path $LinuxJre 'bin\java') -PathType Leaf)) {
        throw "Missing Linux JRE executable"
    }
    & (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp "$Output;$AndroidJar" -d $Output (Join-Path $PSScriptRoot 'BinderFdBridgeTest.java')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & docker run --rm --pull=never --network none `
        -v "${root}:/src:ro" -v "${Output}:/classes:ro" -v "${AndroidJar}:/android.jar:ro" `
        -v "${LinuxJre}:/jre:ro" -e JAVA=/jre/bin/java -e CLASSPATH=/classes:/android.jar `
        -w /src $DockerImage sh tools/shared-microg-binder-test.sh
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
exit 0
