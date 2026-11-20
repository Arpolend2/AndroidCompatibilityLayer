# Run with -Jdk <JDK directory> -AndroidJar <platform android.jar> -Output <scratch directory>.
# This tests the real Megler overlay in two host JVMs, not Android or Aurora IPC.
# The expected missing cross-process objects must change when a real bridge is implemented.
param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$AndroidJar,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
New-Item -ItemType Directory -Force $Output | Out-Null
$sources = @(
    'core\framework-overlay\android\os\Parcel.java',
    'core\framework\src\android\os\Binder.java',
    'core\framework\src\android\os\IBinder.java',
    'core\framework\src\android\os\IInterface.java',
    'core\framework\src\android\os\RemoteException.java',
    'core\framework\src\android\os\ServiceSpecificException.java',
    'core\framework-overlay\megler\Fill.java'
) | ForEach-Object { Join-Path $root $_ }
$sources += Join-Path $PSScriptRoot 'BinderBoundaryTest.java'
& (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp $AndroidJar -d $Output $sources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp "$Output;$AndroidJar" BinderBoundaryTest
exit $LASTEXITCODE
