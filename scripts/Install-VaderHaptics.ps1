[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [string]$PackageDirectory,
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'VaderHaptics'),
    [switch]$Launch
)

$ErrorActionPreference = 'Stop'
$package = (Resolve-Path $PackageDirectory).Path
foreach ($file in @('VaderHaptics.exe', 'vader_ds5_bridge.exe', 'config\m3_calibration.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $file) -PathType Leaf)) {
        throw "Package is incomplete; missing $file"
    }
}

if ($PSCmdlet.ShouldProcess($InstallDirectory, 'Install VaderHaptics package')) {
    New-Item -ItemType Directory -Path $InstallDirectory -Force | Out-Null
    Get-ChildItem -LiteralPath $package -Force |
        Copy-Item -Destination $InstallDirectory -Recurse -Force
    $example = Join-Path $InstallDirectory 'VaderHapticsLauncher.ini.example'
    $settings = Join-Path $InstallDirectory 'VaderHapticsLauncher.ini'
    if ((Test-Path -LiteralPath $example) -and -not (Test-Path -LiteralPath $settings)) {
        Copy-Item -LiteralPath $example -Destination $settings
    }
}

Write-Host "Installed to: $InstallDirectory"
Write-Host 'Open VaderHaptics.exe; select your Flydigi Space Station directory in the launcher if it is not detected.'
if ($Launch) {
    Start-Process -FilePath (Join-Path $InstallDirectory 'VaderHaptics.exe') -Verb RunAs
}
