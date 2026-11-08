<#
.SYNOPSIS
    Compiles the Relight C++ core to WebAssembly (js/relight_core.js) with Emscripten.

.DESCRIPTION
    Same sources as the native library plus the Embind bindings. The output is one ES module
    with the WebAssembly binary embedded, so the static site needs no extra files and no build
    step on the server. Emscripten is activated for this script only.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build_wasm.ps1
#>
[CmdletBinding()]
param(
    [string]$EmsdkDir = (Join-Path $env:USERPROFILE 'emsdk')
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$envScript = Join-Path $EmsdkDir 'emsdk_env.ps1'
if (-not (Test-Path $envScript)) { throw "Emscripten SDK not found in $EmsdkDir (https://emscripten.org/docs/getting_started)." }
$env:EMSDK_QUIET = '1'
. $envScript | Out-Null
if (-not (Get-Command em++ -ErrorAction SilentlyContinue)) { throw 'em++ not found after activating the Emscripten SDK.' }

$sources = @(
    'core/src/camera.cpp', 'core/src/depth.cpp', 'core/src/layers.cpp',
    'core/src/mesh.cpp', 'core/src/gif.cpp', 'core/src/mat4.cpp', 'core/src/shading.cpp',
    'core/wasm/relight_wasm.cpp'
)
$flags = @(
    '-std=c++17', '-O3',
    '-Wall', '-Wextra', '-Wpedantic', '-Wshadow', '-Werror',
    '-fwasm-exceptions',
    '-Icore/include',
    '-lembind',
    '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sEXPORT_NAME=createRelightCore',
    '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=64MB',
    '-sENVIRONMENT=web,worker',
    '-sSINGLE_FILE=1',
    '-o', 'js/relight_core.js'
)

Write-Host "em++ $(em++ --version | Select-Object -First 1)"
& em++ @flags @sources
if ($LASTEXITCODE -ne 0) { throw "em++ failed (exit code $LASTEXITCODE)" }
Write-Host ("wrote js/relight_core.js ({0:N0} KB)" -f ((Get-Item js/relight_core.js).Length / 1KB)) -ForegroundColor Green
