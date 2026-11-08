<#
.SYNOPSIS
    Regenerates every image in docs/images from the live app: the page renders each scene,
    reads it back from the GPU, and the README animation is encoded by the C++ GIF encoder.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\docs_images.ps1
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'headless.ps1')
$r = Invoke-HeadlessPage -Page 'index.html?docs' -Report 'docs-report' -TimeoutSec 300
$r.lines | ForEach-Object { Write-Host $_ }
if (-not $r.ok) { exit 1 }
