param(
    [Parameter(Mandatory=$true)][string]$Jdk,
    [Parameter(Mandatory=$true)][string]$FrameworkClasses,
    [Parameter(Mandatory=$true)][string]$Output
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Output) { throw 'Output directory already exists.' }
New-Item -ItemType Directory -Path $Output | Out-Null
$javac = Join-Path $Jdk 'bin\javac.exe'
$java = Join-Path $Jdk 'bin\java.exe'
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp $FrameworkClasses -d $Output `
    "$PSScriptRoot\megler\Native.java" "$PSScriptRoot\megler\Manifest.java" `
    "$PSScriptRoot\megler\Permissions.java" "$PSScriptRoot\NotificationBridgeTest.java"
if ($LASTEXITCODE -ne 0) { throw 'Notification test compilation failed.' }
& $java -cp "$Output;$FrameworkClasses" NotificationBridgeTest
if ($LASTEXITCODE -ne 0) { throw 'Notification test failed.' }
