param(
    [Parameter(Mandatory=$true)][string]$Jdk,
    [Parameter(Mandatory=$true)][string]$FrameworkClasses,
    [Parameter(Mandatory=$true)][string]$FrameworkStubs,
    [Parameter(Mandatory=$true)][string]$Output
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Output) { throw 'Output directory already exists.' }
New-Item -ItemType Directory -Path $Output | Out-Null
$javac = Join-Path $Jdk 'bin\javac.exe'
$java = Join-Path $Jdk 'bin\java.exe'
$system = Join-Path $Output 'system'
New-Item -ItemType Directory -Path $system | Out-Null
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp "$FrameworkClasses;$FrameworkStubs" -d $system `
    "$PSScriptRoot\..\..\framework\src\android\app\Service.java" `
    "$PSScriptRoot\..\..\framework\src\android\app\NotificationManager.java" `
    "$PSScriptRoot\..\..\framework-overlay\megler\Services.java"
if ($LASTEXITCODE -ne 0) { throw 'System service compilation failed.' }
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp "$system;$FrameworkClasses" -d $Output `
    "$PSScriptRoot\megler\Native.java" "$PSScriptRoot\megler\Manifest.java" `
    "$PSScriptRoot\megler\Permissions.java" "$PSScriptRoot\NotificationBridgeTest.java" `
    "$PSScriptRoot\megler\MicroG.java" "$PSScriptRoot\megler\AppRunner.java" `
    "$PSScriptRoot\ForegroundServiceTest.java" `
    "$PSScriptRoot\SystemServiceDestroyTest.java"
if ($LASTEXITCODE -ne 0) { throw 'Notification test compilation failed.' }
& $java -cp "$Output;$system;$FrameworkClasses" NotificationBridgeTest
if ($LASTEXITCODE -ne 0) { throw 'Notification test failed.' }
& $java -cp "$Output;$system;$FrameworkClasses" ForegroundServiceTest
if ($LASTEXITCODE -ne 0) { throw 'Foreground service notification test failed.' }
& $java -cp "$Output;$system;$FrameworkClasses" SystemServiceDestroyTest
if ($LASTEXITCODE -ne 0) { throw 'System service destruction test failed.' }
