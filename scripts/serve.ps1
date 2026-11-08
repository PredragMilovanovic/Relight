<#
.SYNOPSIS
    Local web server for Relight (static files + a write endpoint for the developer tools).

.DESCRIPTION
    Serves the repository on http://localhost:<Port>/ with correct MIME types and no caching.
    POST /save?path=<p> writes the request body to <p>, but only for paths matching
    samples/<id>.depth.bin, docs/images/<name>.(png|jpg|gif) or .cache/<name>.json, which is
    what the headless tools (?bake, ?docs, tests) produce. Listens on localhost only.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\serve.ps1
    start http://localhost:8040
#>
[CmdletBinding()]
param(
    [int]$Port = 8040,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$rootPrefix = $root.TrimEnd('\') + '\'
$mime = @{
    '.html' = 'text/html; charset=utf-8'; '.js' = 'text/javascript; charset=utf-8'; '.mjs' = 'text/javascript; charset=utf-8'
    '.css' = 'text/css; charset=utf-8'; '.json' = 'application/json'; '.md' = 'text/markdown; charset=utf-8'
    '.png' = 'image/png'; '.jpg' = 'image/jpeg'; '.gif' = 'image/gif'; '.svg' = 'image/svg+xml'
    '.bin' = 'application/octet-stream'; '.wasm' = 'application/wasm'
}
$writable = '^(samples/[a-z0-9_-]+\.depth\.bin|docs/images/[a-z0-9_-]+\.(png|jpg|gif)|\.cache/[a-z0-9_-]+\.json)$'

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
Write-Host "Relight on http://localhost:$Port/  (Ctrl+C to stop)"

try {
    while ($listener.IsListening) {
        $pending = $listener.GetContextAsync()
        while (-not $pending.AsyncWaitHandle.WaitOne(250)) { }   # lets Ctrl+C through
        $ctx = $pending.GetAwaiter().GetResult()
        $req = $ctx.Request
        $res = $ctx.Response
        try {
            $path = [Uri]::UnescapeDataString($req.Url.AbsolutePath).TrimStart('/')
            if ($req.HttpMethod -eq 'POST' -and $path -eq 'save') {
                $target = $req.QueryString['path']
                if ($target -notmatch $writable) {
                    $res.StatusCode = 400
                } else {
                    $full = Join-Path $root $target
                    New-Item -ItemType Directory -Force (Split-Path $full) | Out-Null
                    $fs = [IO.File]::Create($full)
                    try { $req.InputStream.CopyTo($fs) } finally { $fs.Close() }
                    if (-not $Quiet) { Write-Host "saved $target" }
                    $res.StatusCode = 204
                }
            } elseif ($req.HttpMethod -in 'GET', 'HEAD') {
                if ($path -eq '' -or $path.EndsWith('/')) { $path += 'index.html' }
                $full = [IO.Path]::GetFullPath((Join-Path $root $path))
                if (-not $full.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
                    -not (Test-Path -LiteralPath $full -PathType Leaf)) {
                    $res.StatusCode = 404
                } else {
                    $ext = [IO.Path]::GetExtension($full).ToLowerInvariant()
                    $res.ContentType = if ($mime.ContainsKey($ext)) { $mime[$ext] } else { 'application/octet-stream' }
                    $res.Headers.Add('Cache-Control', 'no-store')
                    $bytes = [IO.File]::ReadAllBytes($full)
                    $res.ContentLength64 = $bytes.Length
                    if ($req.HttpMethod -eq 'GET') { $res.OutputStream.Write($bytes, 0, $bytes.Length) }
                }
            } else {
                $res.StatusCode = 405
            }
        } catch {
            if (-not $Quiet) { Write-Warning $_ }
            try { $res.StatusCode = 500 } catch { }
        } finally {
            $res.Close()
        }
    }
} finally {
    $listener.Stop()
}
