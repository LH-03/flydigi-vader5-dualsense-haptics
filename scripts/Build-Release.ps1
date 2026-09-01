[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = 'build',
    [string]$OutputDirectory = 'artifacts'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))

function Find-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $standalone = Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $standalone -PathType Leaf) { return $standalone }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $fromVisualStudio = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
        if ($fromVisualStudio -and (Test-Path -LiteralPath $fromVisualStudio -PathType Leaf)) {
            return $fromVisualStudio
        }
    }
    throw 'CMake was not found. Install CMake 3.20+ or Visual Studio 2022 Desktop C++ tools.'
}

function Find-VsDevCmd([string]$CmakePath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $fromVisualStudio = & $vswhere -latest -products * -find 'Common7\Tools\VsDevCmd.bat' | Select-Object -First 1
        if ($fromVisualStudio -and (Test-Path -LiteralPath $fromVisualStudio -PathType Leaf)) {
            return $fromVisualStudio
        }
    }

    # Visual Studio's CMake bundle is under Common7\IDE\...\CMake\CMake\bin.
    $fromCmakeBundle = Join-Path (Resolve-Path (Join-Path (Split-Path -Parent $CmakePath) '..\..\..\..\..\..')) 'Tools\VsDevCmd.bat'
    if (Test-Path -LiteralPath $fromCmakeBundle -PathType Leaf) {
        return $fromCmakeBundle
    }
    return $null
}

function Find-Ninja([string]$CmakePath) {
    $command = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $bundled = Join-Path (Resolve-Path (Join-Path (Split-Path -Parent $CmakePath) '..\..')) 'Ninja\ninja.exe'
    if (Test-Path -LiteralPath $bundled -PathType Leaf) { return $bundled }
    throw 'Ninja was not found. Install the Visual Studio 2022 C++ desktop tools or Ninja.'
}

function Quote-CmdArgument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Invoke-Cmake([string[]]$Arguments) {
    $quotedArguments = ($Arguments | ForEach-Object { Quote-CmdArgument $_ }) -join ' '
    if ($script:vsDevCmd) {
        $command = '"' + $script:vsDevCmd + '" -arch=x64 -host_arch=x64 && "' + $script:cmake + '" ' + $quotedArguments
        & $env:ComSpec /d /s /c $command
    }
    else {
        & $script:cmake @Arguments
    }
    if ($LASTEXITCODE -ne 0) { throw "CMake failed: $quotedArguments" }
}

function Find-BuiltFile([string]$Name) {
    $candidates = @(
        (Join-Path $build "tools\launcher\$Configuration\$Name"),
        (Join-Path $build "tools\m1\vader_ds5_bridge\$Configuration\$Name"),
        (Join-Path $build "tools\launcher\$Name"),
        (Join-Path $build "tools\m1\vader_ds5_bridge\$Name")
    )
    return $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}

$cmake = Find-CMake
$vsDevCmd = Find-VsDevCmd $cmake
$ninja = Find-Ninja $cmake
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
Invoke-Cmake @('-S', $root, '-B', $build, '-G', 'Ninja', ("-DCMAKE_MAKE_PROGRAM=$ninja"))
Invoke-Cmake @('--build', $build, '--config', $Configuration, '--target', 'VaderHaptics', 'vader_ds5_bridge', 'flydigi_onboard_profile_tests')
& $ctest --test-dir $build -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Offline test run failed.' }

$stage = Join-Path $output 'VaderHaptics'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stage 'config') -Force | Out-Null

$launcher = Find-BuiltFile 'VaderHaptics.exe'
$bridge = Find-BuiltFile 'vader_ds5_bridge.exe'
if (-not $launcher -or -not $bridge) { throw 'Build completed but the launcher or bridge executable was not found.' }

Copy-Item -LiteralPath $launcher -Destination $stage
Copy-Item -LiteralPath $bridge -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'config\m3_calibration.json') -Destination (Join-Path $stage 'config')
Copy-Item -LiteralPath (Join-Path $root 'assets\VaderHapticsLauncher.ini.example') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'README.zh-CN.md') -Destination (Join-Path $stage 'README.zh-CN.md')
Copy-Item -LiteralPath (Join-Path $root 'docs\INSTALL.zh-CN.md') -Destination (Join-Path $stage 'INSTALL.zh-CN.md')
Copy-Item -LiteralPath (Join-Path $root 'NOTICE.md') -Destination $stage

Write-Host "Release package ready: $stage"
