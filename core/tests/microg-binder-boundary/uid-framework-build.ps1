param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$AndroidSdk,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (Test-Path -LiteralPath $Output) { throw "Output must be a new directory: $Output" }
New-Item -ItemType Directory -Path "$Output\stubs", "$Output\classes", "$Output\dex" | Out-Null
$Output = (Resolve-Path $Output).Path
& python "$root\tools\framework-sources.py" "$root\core\framework" "$root\core\framework-overlay" $Output
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$Jdk\bin\javac.exe" -nowarn -source 8 -target 8 -encoding UTF-8 -d "$Output\stubs" "@$Output\stubs.arg"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$Jdk\bin\javac.exe" -nowarn -source 8 -target 8 -encoding UTF-8 -Xmaxerrs 200 `
    -cp "$Output\stubs" -d "$Output\classes" "@$Output\sources.arg"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& python "$root\tools\list-stubs.py" "$Output\classes" "$Output\sources.txt" `
    "$root\core\framework" "$root\core\framework-overlay" "$Output\megler-stubs.txt"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location "$Output\classes"
try { & "$Jdk\bin\jar.exe" cf "$Output\classes.jar" . } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$tools = Join-Path $AndroidSdk 'build-tools\29.0.3'
$jar = Join-Path $AndroidSdk 'platforms\android-29\android.jar'
& "$Jdk\bin\java.exe" -Xmx2g -cp "$tools\lib\d8.jar" com.android.tools.r8.D8 --release `
    --min-api 26 --lib $jar --output "$Output\dex" "$Output\classes.jar"
exit $LASTEXITCODE
