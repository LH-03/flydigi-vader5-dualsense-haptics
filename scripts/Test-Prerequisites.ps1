[CmdletBinding()]
param(
    [string]$SpaceStationDirectory,
    [string]$ViiperPath = (Join-Path $env:LOCALAPPDATA 'VIIPER\viiper.exe'),
    [string]$HidHideCli = 'C:\Program Files\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe'
)

$ErrorActionPreference = 'Stop'
$checks = @(
    [pscustomobject]@{ Name = 'VIIPER'; Passed = Test-Path -LiteralPath $ViiperPath -PathType Leaf; Path = $ViiperPath },
    [pscustomobject]@{ Name = 'HidHide CLI'; Passed = Test-Path -LiteralPath $HidHideCli -PathType Leaf; Path = $HidHideCli },
    [pscustomobject]@{ Name = 'Flydigi Space Station'; Passed = $SpaceStationDirectory -and (Test-Path -LiteralPath (Join-Path $SpaceStationDirectory 'SpaceStationService.exe') -PathType Leaf); Path = $SpaceStationDirectory }
)
$checks | Format-Table -AutoSize
if ($checks.Passed -contains $false) {
    Write-Error 'One or more prerequisites are missing. See docs\DEPENDENCIES.md.'
}
