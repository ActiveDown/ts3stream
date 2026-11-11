# Build helper for the TS3Stream plugin.
# Set TS3STREAM_TOOLCHAIN to the directory containing msvc/, winsdk/, Qt/,
# cmake/, ninja/ and uv/. Default: %USERPROFILE%\ts3stream-toolchain.

$ErrorActionPreference = 'Stop'

$root      = $env:TS3STREAM_TOOLCHAIN
if (-not $root) { $root = Join-Path $HOME 'ts3stream-toolchain' }
$msvc      = Join-Path $root 'msvc'
$sdkBase   = Join-Path $root 'winsdk'
$sdkMain   = Join-Path $sdkBase 'sdk-cpp\c'
$sdkX64    = Join-Path $sdkBase 'microsoft.windows.sdk.cpp.x64.10.0.28000.2957\c'
$sdkBt     = Join-Path $sdkBase 'microsoft.windows.sdk.buildtools.10.0.28000.2957'
$vctools   = Join-Path $msvc 'VC\Tools\MSVC\14.44.35207'
$sdkVer    = '10.0.28000.0'
$qt        = Join-Path $root 'Qt\5.15.2\msvc2019_64'
$cmake     = Join-Path $root 'cmake\cmake-3.31.6-windows-x86_64\bin\cmake.exe'
$ninja     = Join-Path $root 'ninja\ninja.exe'

$env:INCLUDE = "$vctools\include;$sdkMain\Include\$sdkVer\ucrt;$sdkMain\Include\$sdkVer\shared;$sdkMain\Include\$sdkVer\um;$sdkMain\Include\$sdkVer\winrt;$sdkMain\Include\$sdkVer\cppwinrt"
$env:LIB     = "$vctools\lib\x64;$sdkX64\ucrt\x64;$sdkX64\um\x64"
$env:PATH    = "$vctools\bin\Hostx64\x64;$sdkBt\bin\$sdkVer\x64;$qt\bin;$root\uv;$env:PATH"

$source = Split-Path -Parent $MyInvocation.MyCommand.Path
$build  = Join-Path $source 'build'

if (!(Test-Path (Join-Path $source 'third_party\livekit\lib\livekit.lib'))) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $source '..\tools\setup-livekit.ps1')
}

& $cmake -S $source -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_MAKE_PROGRAM="$ninja" `
    -DCMAKE_PREFIX_PATH="$qt" `
    -DCMAKE_C_COMPILER=cl `
    -DCMAKE_CXX_COMPILER=cl

& $cmake --build $build --config Release

$dll = Join-Path $build 'ts3stream_diag.dll'
$helper = Join-Path $build 'ts3stream-audio.exe'
if (Test-Path $dll) {
    $pluginDir = Join-Path $env:APPDATA 'TS3Client\plugins'
    $dataDir = Join-Path $pluginDir 'ts3stream'
    New-Item -ItemType Directory -Force -Path $pluginDir, $dataDir | Out-Null
    Copy-Item $dll (Join-Path $pluginDir 'ts3stream_diag.dll') -Force
    if (Test-Path $helper) {
        Copy-Item $helper (Join-Path $dataDir 'ts3stream-audio.exe') -Force
    }
    Write-Host "Copied to $pluginDir\ts3stream_diag.dll and $dataDir\ts3stream-audio.exe"
    Write-Host "Restart TeamSpeak 3, then check %APPDATA%\TS3Client\plugins\ts3stream\widgets.txt"
} else {
    throw "Build output not found: $dll"
}
