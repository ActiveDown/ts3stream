param(
    [string]$Version = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$plugin = Join-Path $project 'plugin'
$build = Join-Path $plugin 'build'
$dist = Join-Path $project 'dist'
if (-not $Version) {
    $Version = ((Get-Content (Join-Path $plugin 'package.ini') | Select-String '^Version\s*=').Line -replace '^Version\s*=\s*','').Trim()
}
if (-not $Version) { throw 'package version is not set' }
$stage = Join-Path $dist 'staging'

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $plugin 'build.ps1')
    if ($LASTEXITCODE -ne 0) {
        throw 'plugin build failed'
    }
}

Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'plugins\ts3stream') | Out-Null

$packageIni = Get-Content (Join-Path $plugin 'package.ini')
$packageIni = $packageIni -replace '^Version\s*=.*$', "Version = $Version"
Set-Content -Path (Join-Path $stage 'package.ini') -Value $packageIni -Encoding ASCII
Copy-Item (Join-Path $build 'ts3stream_diag.dll') (Join-Path $stage 'plugins\ts3stream_diag.dll') -Force
Copy-Item (Join-Path $build 'ts3stream-audio.exe') (Join-Path $stage 'plugins\ts3stream\ts3stream-audio.exe') -Force
Copy-Item (Join-Path $plugin 'third_party\livekit\bin\livekit.dll') (Join-Path $stage 'plugins\ts3stream\livekit.dll') -Force
Copy-Item (Join-Path $plugin 'third_party\livekit\bin\livekit_ffi.dll') (Join-Path $stage 'plugins\ts3stream\livekit_ffi.dll') -Force
Copy-Item (Join-Path $plugin 'service.json.example') (Join-Path $stage 'plugins\ts3stream\service.json.example') -Force

$readme = @'
TS3Stream

1. Close TeamSpeak 3.
2. Install this package with the TeamSpeak 3 package installer.
3. Start TeamSpeak 3 and connect to a server.
4. The Stream button is enabled only while connected.
5. Control API and LiveKit URL: %APPDATA%\TS3Client\plugins\ts3stream\service.json
   Example: {"controlUrl":"https://stream.example.com","clientKey":""}

The plugin stages livekit.dll and livekit_ffi.dll into the TS3 data directory on
first start. No TeamSpeak client or server binaries are modified.
'@
Set-Content -Path (Join-Path $stage 'plugins\ts3stream\README.txt') -Value $readme -Encoding UTF8

$zip = Join-Path $dist "ts3stream-$Version.zip"
$package = Join-Path $dist "ts3stream-$Version.ts3_plugin"
Remove-Item $zip, $package -Force -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $stageRoot = (Resolve-Path $stage).Path.TrimEnd('\') + '\'
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring($stageRoot.Length).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $_.FullName, $relative,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $archive.Dispose()
}
Move-Item $zip $package -Force

$hash = (Get-FileHash $package -Algorithm SHA256).Hash
Write-Host "package: $package"
Write-Host "sha256:  $hash"
Write-Host "size:    $([math]::Round((Get-Item $package).Length / 1MB, 2)) MB"

