param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$FrameworkClasses,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Output | Out-Null
& (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -cp $FrameworkClasses -d $Output (Join-Path $PSScriptRoot 'StorePhoenixTest.java')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp "$Output;$FrameworkClasses" megler.StorePhoenixTest
exit $LASTEXITCODE
