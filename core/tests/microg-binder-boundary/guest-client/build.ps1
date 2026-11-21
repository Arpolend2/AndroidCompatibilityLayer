param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$AndroidSdk,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Output) { throw "Output must be a new directory: $Output" }
New-Item -ItemType Directory -Path "$Output\classes", "$Output\dex" | Out-Null
$Output = (Resolve-Path $Output).Path
$jar = Join-Path $AndroidSdk 'platforms\android-29\android.jar'
$tools = Join-Path $AndroidSdk 'build-tools\29.0.3'
$source = Join-Path $PSScriptRoot 'src\megler\test\sharedmicrog\ClientApplication.java'
& "$Jdk\bin\javac.exe" -source 8 -target 8 -encoding UTF-8 -bootclasspath "$jar;$tools\core-lambda-stubs.jar" -d "$Output\classes" $source
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location "$Output\classes"
try { & "$Jdk\bin\jar.exe" cf "$Output\classes.jar" . } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$Jdk\bin\java.exe" -cp "$tools\lib\d8.jar" com.android.tools.r8.D8 --release --min-api 26 --lib $jar --output "$Output\dex" "$Output\classes.jar"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($number in 1, 2) {
    $apk = "$Output\client$number.apk"
    & "$tools\aapt2.exe" link -I $jar --manifest "$PSScriptRoot\AndroidManifest.xml" `
        --rename-manifest-package "megler.test.sharedclient$number" -o $apk
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $zip = [System.IO.Compression.ZipFile]::Open($apk, [System.IO.Compression.ZipArchiveMode]::Update)
    try { [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$Output\dex\classes.dex", 'classes.dex') | Out-Null }
    finally { $zip.Dispose() }
}
