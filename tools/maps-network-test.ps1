param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
New-Item -ItemType Directory -Force $Output | Out-Null
$sources = @(Get-ChildItem (Join-Path $root 'core\tests\maps-network') -Recurse -Filter '*.java' |
    ForEach-Object { $_.FullName })
$sources += Join-Path $root 'core\framework-overlay\megler\MapsNetworkDiagnostics.java'
$sources += Join-Path $root 'core\framework-overlay\megler\MapsSystemHttpDiagnostics.java'
& (Join-Path $Jdk 'bin\javac.exe') -source 8 -target 8 -encoding UTF-8 -d $Output $sources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp $Output megler.MapsNetworkDiagnosticsTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp $Output megler.MapsNetworkDiagnosticsTest missing-class
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp $Output megler.MapsNetworkDiagnosticsTest missing-callback
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp $Output megler.MapsNetworkDiagnosticsTest hook-failure
exit $LASTEXITCODE
