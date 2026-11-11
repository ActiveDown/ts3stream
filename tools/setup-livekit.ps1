# Downloads and installs the LiveKit C++ SDK used by the plugin and viewer.
# Run from a normal PowerShell; no admin rights are required.

$ErrorActionPreference = 'Stop'

$root = $env:TS3STREAM_TOOLCHAIN
if (-not $root) { $root = Join-Path $HOME 'ts3stream-toolchain' }
$version = '1.13.1'
$sdkRoot = Join-Path $PSScriptRoot '..\plugin\third_party\livekit'
$zip = Join-Path $root "livekit-sdk-windows-x64-$version.zip"
$extractRoot = Join-Path $root "livekit-sdk-windows-x64-$version"
$url = "https://github.com/livekit/client-sdk-cpp/releases/download/v$version/livekit-sdk-windows-x64-$version.zip"

New-Item -ItemType Directory -Force -Path $root | Out-Null

if (!(Test-Path $zip)) {
    Write-Host "Downloading LiveKit SDK $version..."
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
}

if (!(Test-Path $extractRoot)) {
    Expand-Archive -Path $zip -DestinationPath $extractRoot -Force
}

New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null
Copy-Item -Recurse -Force (Join-Path $extractRoot 'include') (Join-Path $sdkRoot 'include')
Copy-Item -Recurse -Force (Join-Path $extractRoot 'lib') (Join-Path $sdkRoot 'lib')
Copy-Item -Recurse -Force (Join-Path $extractRoot 'bin') (Join-Path $sdkRoot 'bin')

$dataDir = Join-Path $env:APPDATA 'TS3Client\plugins\ts3stream'
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
Copy-Item (Join-Path $extractRoot 'bin\livekit.dll') (Join-Path $dataDir 'livekit.dll') -Force
Copy-Item (Join-Path $extractRoot 'bin\livekit_ffi.dll') (Join-Path $dataDir 'livekit_ffi.dll') -Force

Write-Host "LiveKit SDK installed to $sdkRoot"
Write-Host "Runtime DLLs copied to $dataDir"
