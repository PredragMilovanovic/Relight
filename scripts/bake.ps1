<#
.SYNOPSIS
    Runs the AI depth model on every example photo and stores samples/<id>.depth.bin, so the
    examples open instantly without downloading the model. See docs/samples.md.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\bake.ps1
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'headless.ps1')
$r = Invoke-HeadlessPage -Page 'index.html?bake' -Report 'bake-report' -TimeoutSec 900
$r.lines | ForEach-Object { Write-Host $_ }
if (-not $r.ok) { exit 1 }
