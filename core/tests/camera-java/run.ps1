param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
New-Item -ItemType Directory -Force $Output | Out-Null
$sources = @(Get-ChildItem $PSScriptRoot -Recurse -Filter '*.java' | ForEach-Object { $_.FullName })
$sources += Join-Path $root 'core\framework-overlay\android\media\ImageReader.java'
& (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -d $Output $sources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp $Output CameraImageReaderTest
exit $LASTEXITCODE
