param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$AndroidSdk,
    [Parameter(Mandatory = $true)][string]$AndroidRoot,
    [Parameter(Mandatory = $true)][string]$LinuxJre,
    [Parameter(Mandatory = $true)][string]$FrameworkClasses,
    [Parameter(Mandatory = $true)][string]$Output,
    [string]$MicrogApk,
    [switch]$SelfHostedOnly,
    [string]$DockerImage = 'aurora-build-tools:5.2.1.200'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Jdk = (Resolve-Path $Jdk).Path
$AndroidSdk = (Resolve-Path $AndroidSdk).Path
$AndroidRoot = (Resolve-Path $AndroidRoot).Path
$LinuxJre = (Resolve-Path $LinuxJre).Path
$FrameworkClasses = (Resolve-Path $FrameworkClasses).Path
if ($MicrogApk) { $MicrogApk = (Resolve-Path $MicrogApk).Path }
if (Test-Path $Output) { throw "Output must be a new directory: $Output" }
New-Item -ItemType Directory -Path $Output | Out-Null
$Output = (Resolve-Path $Output).Path
$bt = Join-Path $AndroidSdk 'build-tools\29.0.3'
$jar = Join-Path $AndroidSdk 'platforms\android-29\android.jar'
foreach ($file in @("$Jdk\bin\javac.exe", "$Jdk\bin\jar.exe", "$Jdk\bin\java.exe",
                    "$bt\aapt2.exe", "$bt\lib\d8.jar", "$jar", "$LinuxJre\bin\java")) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing $file" }
}
New-Item -ItemType Directory -Force "$Output\framework-dex", "$Output\framework-update", "$Output\framework-stubs", "$Output\classes",
    "$Output\apk-gen", "$Output\apk-classes", "$Output\apk-dex" | Out-Null
& "$Jdk\bin\javac.exe" -nowarn -source 8 -target 8 -encoding UTF-8 -d "$Output\framework-stubs" `
    "$root\core\framework\stubs\dalvik\system\VMStack.java" `
    "$root\core\framework\stubs\android\system\ErrnoException.java" `
    "$root\core\framework\stubs\android\system\Os.java"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$Jdk\bin\javac.exe" -nowarn -source 8 -target 8 -encoding UTF-8 -cp "$Output\framework-stubs;$FrameworkClasses;$jar" `
    -d "$Output\framework-update" "$root\core\framework-overlay\megler\BinderBridge.java" `
    "$root\core\framework-overlay\megler\SharedMicrogServiceRouter.java" `
    "$root\core\framework-overlay\megler\SharedMicrogIdentity.java" `
    "$root\core\framework-overlay\megler\SharedMicrogConnections.java" `
    "$root\core\framework-overlay\megler\SharedMicrogWorker.java" `
    "$root\core\framework-overlay\megler\SharedMicrogClientStartup.java" `
    "$root\core\framework-overlay\megler\SharedMicrogClientSession.java" `
    "$root\core\framework-overlay\megler\SharedMicrogClientRouter.java" `
    "$root\core\framework-overlay\megler\Services.java" `
    "$root\core\framework-overlay\megler\PhoneApps.java" `
    "$root\core\framework-overlay\megler\AppRunner.java" `
    "$root\core\framework-overlay\android\content\pm\PackageManager.java" `
    "$root\core\framework-overlay\android\os\Process.java" `
    "$root\core\framework\src\android\os\Binder.java" `
    "$root\core\framework-overlay\android\os\RemoteCallbackList.java" `
    "$root\core\framework-overlay\android\os\Parcel.java"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location $FrameworkClasses
try { & "$Jdk\bin\jar.exe" cf "$Output\framework.jar" . } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location "$Output\framework-update"
try { & "$Jdk\bin\jar.exe" uf "$Output\framework.jar" . } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$Jdk\bin\java.exe" -Xmx2g -cp "$bt\lib\d8.jar" com.android.tools.r8.D8 --release --min-api 26 --lib $jar --output "$Output\framework-dex" "$Output\framework.jar"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
foreach ($file in @('classes.dex', 'classes2.dex')) {
    if (!(Test-Path -LiteralPath "$Output\framework-dex\$file")) { throw "Missing framework DEX: $file" }
}
& "$PSScriptRoot\run.ps1" -Jdk $Jdk -AndroidJar $jar -FrameworkClasses $FrameworkClasses -Output "$Output\classes"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$Jdk\bin\javac.exe" -encoding UTF-8 -cp "$Output\classes;$jar" -d "$Output\classes" "$PSScriptRoot\BinderFdBridgeTest.java"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$src = "$root\core\tests\app\binder-fd"
& "$bt\aapt2.exe" compile --dir "$src\res" -o "$Output\res.zip"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$bt\aapt2.exe" link -o "$Output\guest.apk" -I $jar --manifest "$src\AndroidManifest.xml" --java "$Output\apk-gen" "$Output\res.zip"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$sources = @(Get-ChildItem "$src\src", "$Output\apk-gen" -Recurse -Filter '*.java' | ForEach-Object { $_.FullName })
$sources += "$root\core\tests\app\microg\src\com\google\android\gms\common\Feature.java"
& "$Jdk\bin\javac.exe" -nowarn -source 8 -target 8 -encoding UTF-8 -bootclasspath "$jar;$bt\core-lambda-stubs.jar" -cp "$Output\framework-update;$FrameworkClasses" -d "$Output\apk-classes" $sources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location "$Output\apk-classes"
try { & "$Jdk\bin\jar.exe" cf "$Output\guest-classes.jar" . } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$Jdk\bin\java.exe" -cp "$bt\lib\d8.jar" com.android.tools.r8.D8 --release --min-api 26 --lib $jar --output "$Output\apk-dex" "$Output\guest-classes.jar"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open("$Output\guest.apk", [System.IO.Compression.ZipArchiveMode]::Update)
try { [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$Output\apk-dex\classes.dex", 'classes.dex') | Out-Null }
finally { $zip.Dispose() }
& "$bt\aapt2.exe" link -o "$Output\headless.apk" -I $jar --manifest "$src\HeadlessManifest.xml" "$Output\res.zip"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$zip = [System.IO.Compression.ZipFile]::Open("$Output\headless.apk", [System.IO.Compression.ZipArchiveMode]::Update)
try { [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$Output\apk-dex\classes.dex", 'classes.dex') | Out-Null }
finally { $zip.Dispose() }
if ($MicrogApk) {
    Copy-Item $MicrogApk "$Output\microg-test-server.apk"
    $zip = [System.IO.Compression.ZipFile]::Open("$Output\microg-test-server.apk", [System.IO.Compression.ZipArchiveMode]::Update)
    try {
        $n = 2
        while ($zip.GetEntry("classes$n.dex")) { $n++ }
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$Output\apk-dex\classes.dex", "classes$n.dex") | Out-Null
    } finally { $zip.Dispose() }
}
& "$bt\aapt2.exe" link -o "$Output\headless-activity.apk" -I $jar --manifest "$src\ActivityHeadlessManifest.xml" "$Output\res.zip"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$zip = [System.IO.Compression.ZipFile]::Open("$Output\headless-activity.apk", [System.IO.Compression.ZipArchiveMode]::Update)
try { [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$Output\apk-dex\classes.dex", 'classes.dex') | Out-Null }
finally { $zip.Dispose() }
Copy-Item "$Output\headless.apk" "$Output\headless-no-dex.apk"
$zip = [System.IO.Compression.ZipFile]::Open("$Output\headless-no-dex.apk", [System.IO.Compression.ZipArchiveMode]::Update)
try { $zip.GetEntry('classes.dex').Delete() } finally { $zip.Dispose() }
Copy-Item "$Output\headless.apk" "$Output\headless-wrong-abi.apk"
$zip = [System.IO.Compression.ZipFile]::Open("$Output\headless-wrong-abi.apk", [System.IO.Compression.ZipArchiveMode]::Update)
try { $zip.CreateEntry('lib/armeabi-v7a/libfixture.so') | Out-Null } finally { $zip.Dispose() }
& "$bt\aapt2.exe" link -o "$Output\headless-split.apk" -I $jar --manifest "$src\SplitManifest.xml" "$Output\res.zip"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$zip = [System.IO.Compression.ZipFile]::Open("$Output\headless-split.apk", [System.IO.Compression.ZipArchiveMode]::Update)
try { [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$Output\apk-dex\classes.dex", 'classes.dex') | Out-Null }
finally { $zip.Dispose() }
$src = "$root\core\tests\app\gms-broker-mock"
New-Item -ItemType Directory -Force "$Output\broker-gen", "$Output\broker-classes", "$Output\broker-dex" | Out-Null
& "$bt\aapt2.exe" compile --dir "$src\res" -o "$Output\broker-res.zip"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$bt\aapt2.exe" link -o "$Output\broker.apk" -I $jar --manifest "$src\AndroidManifest.xml" --java "$Output\broker-gen" "$Output\broker-res.zip"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$sources = @(Get-ChildItem "$src\src", "$Output\broker-gen" -Recurse -Filter '*.java' | ForEach-Object { $_.FullName })
& "$Jdk\bin\javac.exe" -nowarn -source 8 -target 8 -encoding UTF-8 -bootclasspath "$jar;$bt\core-lambda-stubs.jar" -d "$Output\broker-classes" $sources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location "$Output\broker-classes"
try { & "$Jdk\bin\jar.exe" cf "$Output\broker-classes.jar" . } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$Jdk\bin\java.exe" -cp "$bt\lib\d8.jar" com.android.tools.r8.D8 --release --min-api 26 --lib $jar --output "$Output\broker-dex" "$Output\broker-classes.jar"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$zip = [System.IO.Compression.ZipFile]::Open("$Output\broker.apk", [System.IO.Compression.ZipArchiveMode]::Update)
try { [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$Output\broker-dex\classes.dex", 'classes.dex') | Out-Null }
finally { $zip.Dispose() }
$microgArgs = @()
if ($MicrogApk) { $microgArgs = @('-v', "${MicrogApk}:/real-microg.apk:ro", '-e', 'REAL_GMS_APK=/real-microg.apk') }
if ($SelfHostedOnly) {
    if (!$MicrogApk) { throw 'SelfHostedOnly requires MicrogApk' }
    $microgArgs += @('-e', 'SELF_HOSTED_ONLY=1')
}
& docker run --rm --pull=never --network none --tmpfs /tmp:rw,exec,mode=1777,size=2g @microgArgs `
    -v "${root}:/src:ro" -v "${Output}:/out" -v "${AndroidRoot}:/rootfs:ro" `
    -v "${LinuxJre}:/jre:ro" -v "${jar}:/android.jar:ro" `
    -e WORK_DIR=/tmp/guest-binder -e GUEST_APK=/out/guest.apk -e GUEST_ANDROID_ROOT=/rootfs `
    -e FRAMEWORK_DEX=/out/framework-dex -e JAVA=/jre/bin/java -e CLASSPATH=/out/classes:/android.jar -e RESULT_DIR=/out `
    -w /src $DockerImage sh tools/shared-microg-guest-binder-test.sh
exit $LASTEXITCODE
