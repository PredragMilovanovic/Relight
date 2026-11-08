<#
.SYNOPSIS
    Builds and runs every test: C++ (GoogleTest) and browser (WebAssembly bindings, GPU
    rendering, app behaviour in headless Chrome).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\test.ps1
    powershell -ExecutionPolicy Bypass -File scripts\test.ps1 -SkipNative
#>
[CmdletBinding()]
param([switch]$SkipNative)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'headless.ps1')
Set-Location $RelightRoot
$failed = 0

if (-not $SkipNative) {
    Write-Host '== C++ core (GoogleTest)' -ForegroundColor Cyan
    cmake --preset msvc | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
    cmake --build --preset msvc-release | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'C++ build failed' }
    ctest --preset msvc-release
    if ($LASTEXITCODE -ne 0) { $failed++ }
}

Write-Host '== Browser (headless Chrome)' -ForegroundColor Cyan
$r = Invoke-HeadlessPage -Page 'tests/index.html?report' -Report 'test-report' -TimeoutSec 240
foreach ($line in $r.lines) {
    $color = if ($line -like 'FAIL*') { 'Red' } elseif ($line -like 'PASS*') { 'Green' } else { 'Gray' }
    Write-Host $line -ForegroundColor $color
}
$failed += $r.failed

if ($failed -gt 0) { Write-Host "FAILED ($failed)" -ForegroundColor Red; exit 1 }
Write-Host 'All tests passed' -ForegroundColor Green
