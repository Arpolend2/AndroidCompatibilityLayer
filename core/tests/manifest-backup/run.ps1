param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$Sdk,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Output = [IO.Path]::GetFullPath($Output)
$javac = Join-Path $Jdk 'bin\javac.exe'
$java = Join-Path $Jdk 'bin\java.exe'
$aapt = Join-Path $Sdk 'build-tools\29.0.3\aapt2.exe'
$android = Join-Path $Sdk 'platforms\android-29\android.jar'
New-Item -ItemType Directory -Force $Output, "$Output\classes", "$Output\stubs", "$Output\res\values" | Out-Null
@'
<resources><bool name="backup_false">false</bool><bool name="backup_true">true</bool></resources>
'@ | Set-Content -Encoding UTF8 "$Output\res\values\bools.xml"
& $aapt compile --dir "$Output\res" -o "$Output\res.zip"
if ($LASTEXITCODE -ne 0) { throw 'aapt2 compile failed' }
$cases = @{
    'false' = ' android:allowBackup="false"'
    'true' = ' android:allowBackup="true"'
    'default' = ''
    'reference-false' = ' android:allowBackup="@bool/backup_false"'
    'reference-true' = ' android:allowBackup="@bool/backup_true"'
}
foreach ($name in $cases.Keys) {
    $manifest = '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="megler.test.backup" android:installLocation="internalOnly"><application' + $cases[$name] + ' /></manifest>'
    [IO.File]::WriteAllText("$Output\manifest.xml", $manifest, [Text.UTF8Encoding]::new($false))
    & $aapt link -o "$Output\$name.apk" -I $android --manifest "$Output\manifest.xml" "$Output\res.zip"
    if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed: $name" }
}
& python (Join-Path $root 'tools\framework-sources.py') (Join-Path $root 'core\framework') (Join-Path $root 'core\framework-overlay') $Output
if ($LASTEXITCODE -ne 0) { throw 'framework source selection failed' }
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -d "$Output\stubs" "@$Output\stubs.arg"
if ($LASTEXITCODE -ne 0) { throw 'stub compilation failed' }
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp "$Output\stubs" -d "$Output\classes" "@$Output\sources.arg"
if ($LASTEXITCODE -ne 0) { throw 'framework compilation failed' }
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp "$Output\classes;$Output\stubs" -d "$Output\classes" "$PSScriptRoot\Native.java" "$PSScriptRoot\ManifestBackupTest.java"
if ($LASTEXITCODE -ne 0) { throw 'test compilation failed' }
foreach ($variant in @('overlay', 'original')) {
    if ($variant -eq 'original') {
        & $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp "$Output\classes;$Output\stubs" -d "$Output\classes" (Join-Path $root 'core\framework\src\android\content\pm\ApplicationInfo.java')
        if ($LASTEXITCODE -ne 0) { throw 'original ApplicationInfo compilation failed' }
    }
    foreach ($name in @('false', 'true', 'default', 'reference-false', 'reference-true')) {
        $expected = $name -ne 'false' -and $name -ne 'reference-false'
        & $java "-Dtest.apk=$Output\$name.apk" -cp "$Output\classes;$Output\stubs" ManifestBackupTest $Output "$expected" "$variant/$name"
        if ($LASTEXITCODE -ne 0) { throw "regression failed: $variant/$name" }
    }
}
