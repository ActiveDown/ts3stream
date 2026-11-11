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
$cmake     = Join-Path $root 'cmake\cmake-3.31.6-windows-x86_64\bin\cmake.exe'
$ninja     = Join-Path $root 'ninja\ninja.exe'

$env:INCLUDE = "$vctools\include;$sdkMain\Include\$sdkVer\ucrt;$sdkMain\Include\$sdkVer\shared;$sdkMain\Include\$sdkVer\um;$sdkMain\Include\$sdkVer\winrt;$sdkMain\Include\$sdkVer\cppwinrt"
$env:LIB     = "$vctools\lib\x64;$sdkX64\ucrt\x64;$sdkX64\um\x64"
$env:PATH    = "$vctools\bin\Hostx64\x64;$sdkBt\bin\$sdkVer\x64;$env:PATH"

$source = Split-Path -Parent $MyInvocation.MyCommand.Path
$build  = Join-Path $source 'build'

& $cmake -S $source -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_MAKE_PROGRAM="$ninja" `
    -DCMAKE_CXX_COMPILER=cl

& $cmake --build $build --config Release
Write-Host "Run: $build\wgc_test.exe"
