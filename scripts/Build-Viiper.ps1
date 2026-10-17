[CmdletBinding()]
param(
    [string]$GoExe = 'go',
    [string]$WorkDirectory = 'build/viiper-source',
    [string]$OutputDirectory = 'runtime'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$work = [IO.Path]::GetFullPath((Join-Path $root $WorkDirectory))
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$revision = '3a7f183699d4b508b028abfd17889fb189cd7950'
$source = Join-Path $work "VIIPER-$revision"
$archive = Join-Path $work 'VIIPER-source.zip'
New-Item -ItemType Directory -Path $work,$output -Force | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $source 'go.mod'))) {
    Invoke-WebRequest -UseBasicParsing -Uri "https://codeload.github.com/hbashton/VIIPER/zip/$revision" -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath $work -Force
}
foreach ($patch in @('input-queue.patch','capability-marker.patch')) {
    $path = Join-Path $root "third-party/viiper/$patch"
    $ErrorActionPreference = 'Continue'
    & git -C $source apply --reverse --check $path 2>$null
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -ne 0) {
        & git -C $source apply --check $path
        if ($LASTEXITCODE -ne 0) { throw "Patch does not match pinned source: $patch" }
        & git -C $source apply $path
        if ($LASTEXITCODE -ne 0) { throw "Patch failed: $patch" }
    }
}
Push-Location $source
try {
    & $GoExe test ./device/dualsense ./internal/server/api
    if ($LASTEXITCODE -ne 0) { throw 'VIIPER input/runtime tests failed.' }
    & $GoExe build -trimpath -ldflags '-s -w' -o (Join-Path $output 'viiper.exe') ./cmd/viiper
    if ($LASTEXITCODE -ne 0) { throw 'VIIPER build failed.' }
} finally { Pop-Location }
Copy-Item -LiteralPath (Join-Path $source 'LICENSE.txt') -Destination (Join-Path $output 'VIIPER-LICENSE.txt')
if (Test-Path -LiteralPath $archive) { Copy-Item -LiteralPath $archive -Destination (Join-Path $output 'VIIPER-source.zip') }
New-Item -ItemType Directory -Path (Join-Path $output 'source-patches') -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $root 'third-party/viiper') -File | Copy-Item -Destination (Join-Path $output 'source-patches') -Force
@("VIIPER upstream: https://github.com/hbashton/VIIPER", "Revision: $revision", 'Input queue patch: I7, capacity 8, endpoint reset/stream end fixes.', 'Capability marker: vaderhapticsInputRevision=1; inputQueueCapacity=8.', 'Rebuild: scripts/Build-Viiper.ps1 with Go 1.26.2 or compatible toolchain.') | Set-Content -LiteralPath (Join-Path $output 'VIIPER-SOURCE.txt') -Encoding utf8
Write-Host "Compatible runtime built at: $output"
