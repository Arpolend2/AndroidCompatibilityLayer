param(
    [Parameter(Mandatory = $true)][string]$Jdk,
    [Parameter(Mandatory = $true)][string]$AndroidJar,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$framework = Join-Path $Output 'framework'
$stubs = Join-Path $framework 'stubs'
$classes = Join-Path $framework 'classes'
$testClasses = Join-Path $Output 'test-classes'
foreach ($directory in @($stubs, $classes, $testClasses)) {
    if (Test-Path $directory) { Remove-Item -Recurse -Force $directory }
    New-Item -ItemType Directory -Force $directory | Out-Null
}

$frameworkSources = Join-Path $root 'tools\framework-sources.py'
& python $frameworkSources (Join-Path $root 'core\framework') (Join-Path $root 'core\framework-overlay') $framework
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$javac = Join-Path $Jdk 'bin\javac.exe'
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -d $stubs ('@' + (Join-Path $framework 'stubs.arg'))
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -Xmaxerrs 200 -cp ($stubs + ';' + $AndroidJar) -d $classes ('@' + (Join-Path $framework 'sources.arg'))
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$testSources = @(
    (Join-Path $root 'core\tests\microg-binder-boundary\SharedMicrogClientRouterTest.java'),
    (Join-Path $root 'core\tests\microg-binder-boundary\SharedMicrogClientSessionTest.java')
)
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp ($classes + ';' + $AndroidJar) -d $testClasses $testSources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& (Join-Path $Jdk 'bin\java.exe') -cp ($testClasses + ';' + $classes + ';' + $AndroidJar) megler.SharedMicrogClientRouterTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp ($testClasses + ';' + $classes + ';' + $AndroidJar) megler.SharedMicrogClientSessionTest
exit $LASTEXITCODE
