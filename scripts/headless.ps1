# Shared helpers for the headless tools (test.ps1, bake.ps1, docs_images.ps1). Dot-source it.
#
# Starts scripts/serve.ps1 and a headless Chrome (or Edge) on a page; the page does the work and
# POSTs a JSON report to .cache/<name>.json, which these helpers wait for. No Node.js, Python or
# browser-automation library is needed.

$script:RelightRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))

function Find-Browser {
    $candidates = @(
        $env:CHROME,
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
    )
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    throw 'Chrome or Edge not found; set the CHROME environment variable to its path.'
}

function Invoke-HeadlessPage {
    param(
        [Parameter(Mandatory)] [string]$Page,      # e.g. 'tests/index.html?report'
        [Parameter(Mandatory)] [string]$Report,    # e.g. 'test-report' -> .cache/test-report.json
        [int]$TimeoutSec = 300,
        [int]$Port = 8047,
        [string]$Size = '1600,900'
    )
    $cache = Join-Path $script:RelightRoot '.cache'
    New-Item -ItemType Directory -Force $cache | Out-Null
    $reportFile = Join-Path $cache "$Report.json"
    Remove-Item $reportFile -ErrorAction SilentlyContinue

    $server = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'serve.ps1'), '-Port', $Port, '-Quiet')
    $browser = $null
    try {
        $deadline = (Get-Date).AddSeconds(15)
        while ($true) {
            try { Invoke-WebRequest "http://localhost:$Port/index.html" -UseBasicParsing -TimeoutSec 2 | Out-Null; break }
            catch { if ((Get-Date) -gt $deadline) { throw 'local server did not start' }; Start-Sleep -Milliseconds 200 }
        }
        # Own profile (keeps the AI model cached between runs), WebGPU allowed.
        $browserArgs = @(
            '--headless=new', '--remote-debugging-port=0', "--user-data-dir=$(Join-Path $cache 'chrome-profile')",
            '--no-first-run', '--no-default-browser-check', '--enable-unsafe-webgpu', '--ignore-gpu-blocklist',
            "--window-size=$Size", "http://localhost:$Port/$Page")
        $browser = Start-Process (Find-Browser) -PassThru -ArgumentList $browserArgs
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while (-not (Test-Path $reportFile)) {
            if ((Get-Date) -gt $deadline) { throw "no report from $Page after $TimeoutSec s" }
            Start-Sleep -Milliseconds 300
        }
        Start-Sleep -Milliseconds 200   # let the write finish
        return Get-Content $reportFile -Raw | ConvertFrom-Json
    } finally {
        if ($browser) { & taskkill /PID $browser.Id /T /F 2>$null | Out-Null }
        if ($server) { & taskkill /PID $server.Id /T /F 2>$null | Out-Null }
    }
}
