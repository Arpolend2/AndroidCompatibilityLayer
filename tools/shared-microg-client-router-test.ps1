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
$orderedClasses = Join-Path $Output 'ordered-test-classes'
$bundleClasses = Join-Path $Output 'bundle-test-classes'
$seekClasses = Join-Path $Output 'seek-test-classes'
$reliableClasses = Join-Path $Output 'reliable-test-classes'
foreach ($directory in @($stubs, $classes, $testClasses, $orderedClasses, $bundleClasses, $seekClasses, $reliableClasses)) {
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
    (Join-Path $root 'core\tests\microg-binder-boundary\SharedMicrogClientSessionTest.java'),
    (Join-Path $root 'core\tests\microg-binder-boundary\SharedMicrogModuleOnlyTest.java')
)
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp ($classes + ';' + $AndroidJar) -d $testClasses $testSources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& (Join-Path $Jdk 'bin\java.exe') -cp ($testClasses + ';' + $classes + ';' + $AndroidJar) megler.SharedMicrogClientRouterTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp ($testClasses + ';' + $classes + ';' + $AndroidJar) megler.SharedMicrogClientSessionTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp ($testClasses + ';' + $classes + ';' + $AndroidJar) megler.SharedMicrogModuleOnlyTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$orderedSources = @(
    (Join-Path $root 'core\tests\microg-binder-boundary\ordered-host-stubs\megler\Native.java'),
    (Join-Path $root 'core\tests\microg-binder-boundary\SharedMicrogOrderedDispatchTest.java')
)
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp ($classes + ';' + $AndroidJar) -d $orderedClasses $orderedSources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp ($orderedClasses + ';' + $classes + ';' + $AndroidJar) megler.SharedMicrogOrderedDispatchTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$bundleSources = @(
    (Join-Path $root 'core\tests\microg-binder-boundary\BinderBoundaryTest.java'),
    (Join-Path $root 'core\tests\microg-binder-boundary\BinderBundleBridgeTest.java')
)
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp ($classes + ';' + $AndroidJar) -d $bundleClasses $bundleSources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp ($bundleClasses + ';' + $classes + ';' + $AndroidJar) BinderBundleBridgeTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp ($bundleClasses + ';' + $classes + ';' + $AndroidJar) BinderBoundaryTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$seekSources = @(
    (Join-Path $root 'core\tests\microg-binder-boundary\fd-seek-host-stubs\android\system\Os.java'),
    (Join-Path $root 'core\tests\microg-binder-boundary\ParcelFileDescriptorSeekTest.java')
)
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp ($classes + ';' + $AndroidJar) -d $seekClasses $seekSources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') -cp ($seekClasses + ';' + $classes + ';' + $AndroidJar) ParcelFileDescriptorSeekTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$reliableSources = @(
    (Join-Path $root 'core\tests\microg-binder-boundary\reliable-host-stubs\android\system\ErrnoException.java'),
    (Join-Path $root 'core\tests\microg-binder-boundary\reliable-host-stubs\android\system\Os.java'),
    (Join-Path $root 'core\tests\microg-binder-boundary\reliable-host-stubs\megler\SharedMicrogFdTransport.java'),
    (Join-Path $root 'core\tests\microg-binder-boundary\ParcelFileDescriptorReliableTest.java'),
    (Join-Path $root 'core\tests\microg-binder-boundary\BinderBridgeFdTest.java')
)
& $javac -nowarn -source 8 -target 8 -encoding UTF-8 -cp ($classes + ';' + $AndroidJar) -d $reliableClasses $reliableSources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') --add-opens=java.base/java.io=ALL-UNNAMED -cp ($reliableClasses + ';' + $classes + ';' + $AndroidJar) ParcelFileDescriptorReliableTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $Jdk 'bin\java.exe') --add-opens=java.base/java.io=ALL-UNNAMED -cp ($reliableClasses + ';' + $classes + ';' + $AndroidJar) megler.BinderBridgeFdTest
exit $LASTEXITCODE
