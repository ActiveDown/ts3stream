[CmdletBinding()]
param(
    [string]$WorkDir = (Join-Path $env:TEMP ('ts3stream-livekit-' + [guid]::NewGuid().ToString('N'))),
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\..\dist')
)
$ErrorActionPreference = 'Stop'
$pin = Get-Content (Join-Path $PSScriptRoot 'upstream.json') -Raw | ConvertFrom-Json
if (Test-Path $WorkDir) { throw 'WorkDir must not exist: use a fresh directory to avoid overwriting source changes.' }
git clone --depth 1 --branch $pin.tag $pin.repository $WorkDir
if ($LASTEXITCODE -ne 0) { throw 'Clone failed' }
if ((git -C $WorkDir rev-parse HEAD) -ne $pin.commit) { throw 'Upstream commit mismatch' }
$patch = Join-Path $PSScriptRoot $pin.patch
git -C $WorkDir apply --check $patch
if ($LASTEXITCODE -ne 0) { throw 'Patch validation failed' }
git -C $WorkDir apply $patch
if ($LASTEXITCODE -ne 0) { throw 'Patch application failed' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path
$oldOS = $env:GOOS; $oldArch = $env:GOARCH; $oldCGO = $env:CGO_ENABLED
Push-Location $WorkDir
try {
    $env:CGO_ENABLED = '0'
    Remove-Item Env:GOOS -ErrorAction SilentlyContinue
    go test ./pkg/sfu ./pkg/sfu/buffer -run '^TestServerAV' -count=1
    if ($LASTEXITCODE -ne 0) { throw 'Server buffer regression failed' }
    $env:GOARCH = 'amd64'
    foreach ($os in @('windows','linux')) {
        $env:GOOS = $os
        $name = if ($os -eq 'windows') { 'ts3stream-livekit.exe' } else { 'ts3stream-livekit-linux-amd64' }
        go build -trimpath -ldflags '-s -w' -o (Join-Path $OutputDir $name) ./cmd/server
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $os" }
    }
} finally {
    Pop-Location
    $env:GOOS = $oldOS; $env:GOARCH = $oldArch; $env:CGO_ENABLED = $oldCGO
}
