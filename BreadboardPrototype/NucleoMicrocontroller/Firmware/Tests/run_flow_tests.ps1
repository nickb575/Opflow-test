$ErrorActionPreference = 'Stop'
$firmwareRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ build tools are required.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'Visual Studio C++ build tools were not found.' }
$devCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$outputDir = Join-Path $firmwareRoot 'build/flow-host-tests'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$includes = Join-Path $firmwareRoot 'App/Inc'
$source = Join-Path $firmwareRoot 'App/Src/Paw3902.cpp'
$fake = Join-Path $PSScriptRoot 'FakeFlowBus.cpp'
$tests = Join-Path $PSScriptRoot 'Paw3902Tests.cpp'
$command = 'call "' + $devCommand + '" -no_logo -arch=x64 && cl.exe /nologo /std:c++17 /W4 /WX /EHsc /I"' + $includes + '" "' + $source + '" "' + $fake + '" "' + $tests + '" /Fe:Paw3902Tests.exe /link /INCREMENTAL:NO && Paw3902Tests.exe'
Push-Location -LiteralPath $outputDir
try {
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw 'PAW3902 tests failed.' }
} finally { Pop-Location }
