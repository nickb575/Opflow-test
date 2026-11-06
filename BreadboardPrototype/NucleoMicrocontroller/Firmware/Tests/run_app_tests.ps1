$ErrorActionPreference = 'Stop'

$firmwareRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Install the Visual Studio C++ build tools to run the native application tests.'
}
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) {
    throw 'Visual Studio C++ build tools were not found.'
}

$devCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$outputDir = Join-Path $firmwareRoot 'build/host-tests'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$fakeIncludes = Join-Path $PSScriptRoot 'Fakes'
$appIncludes = Join-Path $firmwareRoot 'App/Inc'
$appSource = Join-Path $firmwareRoot 'App/Src/App.cpp'
$testSource = Join-Path $PSScriptRoot 'AppTests.cpp'
$imuSource = Join-Path $firmwareRoot 'App/Src/Mpu6050.cpp'
$busFake = Join-Path $PSScriptRoot 'FakeImuBus.cpp'
$imuTests = Join-Path $PSScriptRoot 'Mpu6050Tests.cpp'
$flowSource = Join-Path $firmwareRoot 'App/Src/Paw3902.cpp'
$flowFake = Join-Path $PSScriptRoot 'FakeFlowBus.cpp'
$tofFake = Join-Path $PSScriptRoot 'FakeAfbrS50.cpp'
$compile = 'cl.exe /nologo /std:c++17 /W4 /WX /EHsc /I"' + $fakeIncludes + '" /I"' + $appIncludes + '" '
$common = '"' + $imuSource + '" "' + $busFake + '" '
$command = 'call "' + $devCommand + '" -no_logo -arch=x64 && ' + $compile + $common + '"' + $flowSource + '" "' + $flowFake + '" "' + $tofFake + '" "' + $appSource + '" "' + $testSource + '" /Fe:AppTests.exe /link /INCREMENTAL:NO && AppTests.exe && ' + $compile + $common + '"' + $imuTests + '" /Fe:Mpu6050Tests.exe /link /INCREMENTAL:NO && Mpu6050Tests.exe'

Push-Location -LiteralPath $outputDir
try {
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw 'Application tests failed.' }
} finally {
    Pop-Location
}
& (Join-Path $PSScriptRoot 'run_flow_tests.ps1')
