# Builds two synthetic APKs and runs them in the guest runtime with networking disabled.
# Without FrameworkDexDirectory, builds the current framework into Output as well.
param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$AndroidSdk,
    [string]$FrameworkDexDirectory,
    [Parameter(Mandatory = $true)][string]$AndroidRoot,
    [Parameter(Mandatory = $true)][string]$Runner,
    [Parameter(Mandatory = $true)][string]$Output,
    [string]$DockerImage = 'aurora-build-tools:5.2.1.200'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Jdk = (Resolve-Path $Jdk).Path
$AndroidSdk = (Resolve-Path $AndroidSdk).Path
if ($FrameworkDexDirectory) { $FrameworkDexDirectory = (Resolve-Path $FrameworkDexDirectory).Path }
$AndroidRoot = (Resolve-Path $AndroidRoot).Path
$Runner = (Resolve-Path $Runner).Path
if (Test-Path $Output) { throw "Output must be a new directory: $Output" }
New-Item -ItemType Directory -Path $Output | Out-Null
$Output = (Resolve-Path $Output).Path
$bt = Join-Path $AndroidSdk 'build-tools\29.0.3'
$jar = Join-Path $AndroidSdk 'platforms\android-29\android.jar'
foreach ($file in @("$Jdk\bin\javac.exe", "$Jdk\bin\jar.exe", "$Jdk\bin\java.exe",
                    "$bt\aapt2.exe", "$bt\lib\d8.jar", "$jar")) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing $file" }
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Native tools write informational output to stderr; exit status, not PowerShell's stderr conversion, decides success.
$ErrorActionPreference = 'Continue'
if (!$FrameworkDexDirectory) {
    $framework = Join-Path $Output 'framework'
    New-Item -ItemType Directory -Force "$framework\stubs", "$framework\classes", "$framework\dex" | Out-Null
    & python "$root\tools\framework-sources.py" "$root\core\framework" "$root\core\framework-overlay" $framework
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & "$Jdk\bin\javac.exe" -nowarn -source 8 -target 8 -encoding UTF-8 -d "$framework\stubs" "@$framework\stubs.arg" 2> "$framework\stubs.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$framework\stubs.log" -TotalCount 40; exit $LASTEXITCODE }
    & "$Jdk\bin\javac.exe" -nowarn -source 8 -target 8 -encoding UTF-8 -Xmaxerrs 200 -cp "$framework\stubs" -d "$framework\classes" "@$framework\sources.arg" 2> "$framework\javac.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$framework\javac.log" -TotalCount 60; exit $LASTEXITCODE }
    Push-Location "$framework\classes"
    try { & "$Jdk\bin\jar.exe" cf "$framework\classes.jar" . } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & "$Jdk\bin\java.exe" -Xmx2g -cp "$bt\lib\d8.jar" com.android.tools.r8.D8 --release --min-api 26 --lib $jar --output "$framework\dex" "$framework\classes.jar" 2> "$framework\d8.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$framework\d8.log" -TotalCount 40; exit $LASTEXITCODE }
    $FrameworkDexDirectory = "$framework\dex"
}
foreach ($file in @("$FrameworkDexDirectory\classes.dex", "$FrameworkDexDirectory\classes2.dex")) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing $file" }
}
foreach ($app in @('gms-broker-mock', 'gms-broker-client')) {
    $src = Join-Path $root "core\tests\app\$app"
    $dir = Join-Path $Output $app
    New-Item -ItemType Directory -Force "$dir\gen", "$dir\classes", "$dir\dex" | Out-Null
    & "$bt\aapt2.exe" compile --dir "$src\res" -o "$dir\res.zip"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & "$bt\aapt2.exe" link -o "$Output\$app.apk" -I $jar --manifest "$src\AndroidManifest.xml" --java "$dir\gen" "$dir\res.zip"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $sources = @(Get-ChildItem "$src\src", "$dir\gen" -Recurse -Filter '*.java' | ForEach-Object { $_.FullName })
    & "$Jdk\bin\javac.exe" -nowarn -source 8 -target 8 -encoding UTF-8 -bootclasspath "$jar;$bt\core-lambda-stubs.jar" -d "$dir\classes" $sources
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Push-Location "$dir\classes"
    try { & "$Jdk\bin\jar.exe" cf "$dir\classes.jar" . } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & "$Jdk\bin\java.exe" -cp "$bt\lib\d8.jar" com.android.tools.r8.D8 --release --min-api 26 --lib $jar --output "$dir\dex" "$dir\classes.jar"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $zip = [System.IO.Compression.ZipFile]::Open("$Output\$app.apk", [System.IO.Compression.ZipArchiveMode]::Update)
    try { [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$dir\dex\classes.dex", 'classes.dex') | Out-Null }
    finally { $zip.Dispose() }
}
$command = 'cp -a /rootfs /tmp/android-root; cp /dex/classes.dex /tmp/android-root/framework/megler-framework.dex; cp /dex/classes2.dex /tmp/android-root/framework/megler-framework-classes2.dex; mkdir /tmp/app-data; MG_JAVA_GMS_APK=/apks/gms-broker-mock.apk timeout 90 /runner --java app /apks/gms-broker-client.apk /tmp/app-data /tmp/android-root - 1 800x480@160 wait:10000 > /apks/guest.log 2>&1; result=$?; grep GmsBrokerTest /apks/guest.log; grep -q GmsBrokerTest.*PASS /apks/guest.log || exit 1; grep -q GmsBrokerTest.*FAIL /apks/guest.log && exit 1; exit $result'
& docker run --rm --pull=never --network none --tmpfs /tmp:rw,exec,mode=1777,size=1g `
    -v "${AndroidRoot}:/rootfs:ro" -v "${FrameworkDexDirectory}:/dex:ro" `
    -v "${Output}:/apks" -v "${Runner}:/runner:ro" $DockerImage sh -c $command
exit $LASTEXITCODE
