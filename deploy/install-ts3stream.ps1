#Requires -Version 5.1
<#
.SYNOPSIS
  TS3Stream installer for Windows servers.

.DESCRIPTION
  Installs LiveKit, ts3stream-control and (optionally) Caddy with TLS.
  The stock TeamSpeak 3 server is NOT installed or modified.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install-ts3stream.ps1

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install-ts3stream.ps1 -Domain stream.example.com -Yes

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install-ts3stream.ps1 -Domain 192.168.1.50 -NoTls -Yes
#>
[CmdletBinding()]
param(
    [string]$Domain,
    [string]$ApiKey = 'ts3stream',
    [string]$ApiSecret,
    [string]$ClientKey,
    [string]$Version,
    [ValidateRange(0,5000)][int]$PlayoutDelayMs = 2000,
    [string]$InstallDir = 'C:\TS3Stream',
    [string]$Ts3Dir = 'C:\TS3Server',
    [string]$Ts3QueryPassword,
    [switch]$NoTls,
    [switch]$SkipFirewall,
    [switch]$NoServices,
    [switch]$Yes
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$Repo = 'ActiveDown/ts3stream'
$LogDir = Join-Path $InstallDir 'logs'

# A reinstall must not silently enable buffering for older viewer plugins.
if (-not $PSBoundParameters.ContainsKey('PlayoutDelayMs') -and
    (Test-Path (Join-Path $InstallDir 'start-control.cmd'))) {
    $previous = Get-Content (Join-Path $InstallDir 'start-control.cmd') -Raw
    $PlayoutDelayMs = 0
    if ($previous -match '(?m)^set "TS3STREAM_PLAYOUT_DELAY_MS=(\d+)"\s*$') {
        $PlayoutDelayMs = [int]$Matches[1]
    }
}
if ($PlayoutDelayMs -lt 0 -or $PlayoutDelayMs -gt 5000 -or $PlayoutDelayMs % 10 -ne 0) {
    throw 'PlayoutDelayMs must be 0..5000 in steps of 10 milliseconds.'
}

function Write-Step([string]$Message) {
    Write-Host "[ts3stream] $Message" -ForegroundColor Cyan
}
function Write-Warn([string]$Message) {
    Write-Host "[warning] $Message" -ForegroundColor Yellow
}
function Fail([string]$Message) {
    Write-Host "[error] $Message" -ForegroundColor Red
    exit 1
}
function New-Secret([int]$Bytes) {
    $buffer = New-Object byte[] $Bytes
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($buffer) } finally { $rng.Dispose() }
    return (($buffer | ForEach-Object { $_.ToString('x2') }) -join '')
}
function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Administrator)) {
    Fail 'Run this installer from an elevated PowerShell (Run as Administrator).'
}

if ([string]::IsNullOrWhiteSpace($Domain)) {
    $Domain = Read-Host 'Public DNS name or LAN IP (for example stream.example.com)'
}
$Domain = $Domain.Trim()
$Domain = $Domain -replace '^https?://', ''
$Domain = $Domain.TrimEnd('/')
if ([string]::IsNullOrWhiteSpace($Domain)) {
    Fail 'Domain must not be empty.'
}
if (-not $NoTls -and $Domain -match '^\d{1,3}(\.\d{1,3}){3}$') {
    Fail 'Caddy/TLS needs a DNS name, not an IP. Use -NoTls for LAN/IP installation.'
}
if ([string]::IsNullOrWhiteSpace($ApiSecret)) { $ApiSecret = New-Secret 32 }
if ([string]::IsNullOrWhiteSpace($ClientKey)) { $ClientKey = New-Secret 64 }

Write-Step "Install directory: $InstallDir"
Write-Step "TLS/Caddy: $(-not $NoTls)"
Write-Step "A/V buffer: $PlayoutDelayMs ms (requires viewer plugin 0.3.8+)"
if (-not $Yes) {
    Write-Host ''
    Write-Host 'Install plan:'
    Write-Host "  domain:      $Domain"
    Write-Host "  TLS/Caddy:   $(-not $NoTls)"
    Write-Host "  services:    $(-not $NoServices)"
    Write-Host "  firewall:    $(-not $SkipFirewall)"
    $answer = Read-Host 'Continue? [Y/n]'
    if ($answer -match '^(n|no)$') { Fail 'Cancelled.' }
}

New-Item -ItemType Directory -Force -Path $InstallDir, $LogDir | Out-Null

# Auto-detect the ServerQuery password from the stock TS3 server logs.
if ([string]::IsNullOrWhiteSpace($Ts3QueryPassword)) {
    $logCandidates = @()
    if (Test-Path (Join-Path $Ts3Dir 'logs')) {
        $logCandidates += Get-ChildItem (Join-Path $Ts3Dir 'logs') -Filter '*.log' -ErrorAction SilentlyContinue
    }
    $logCandidates += Get-ChildItem $Ts3Dir -Filter '*.out' -ErrorAction SilentlyContinue
    $logCandidates += Get-ChildItem $Ts3Dir -Filter '*.err' -ErrorAction SilentlyContinue
    foreach ($log in $logCandidates) {
        $match = Select-String -Path $log.FullName -Pattern 'password=\s*"([^"]+)"' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($match) {
            $Ts3QueryPassword = $match.Matches[0].Groups[1].Value
            Write-Step "Detected TS3 ServerQuery password in $($log.FullName)"
            break
        }
    }
}
if ([string]::IsNullOrWhiteSpace($Ts3QueryPassword)) {
    Write-Warn 'TS3 ServerQuery password not found. Auto-provisioning/UID verification will be disabled.'
    if (-not $Yes) {
        $entered = Read-Host 'Enter the current ServerQuery password (leave empty to skip auto-provisioning)'
        if (-not [string]::IsNullOrWhiteSpace($entered)) {
            $Ts3QueryPassword = $entered
            Write-Step 'Using the entered ServerQuery password'
        }
    }
}
if ([string]::IsNullOrWhiteSpace($Ts3QueryPassword)) {
    Write-Warn "Pass -Ts3QueryPassword or ensure $Ts3Dir\logs contains the first-run server log."
}

function Download-File([string]$Url, [string]$Destination) {
    Write-Step "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
}

function Expand-Zip([string]$Zip, [string]$Destination) {
    if (Test-Path $Destination) { Remove-Item $Destination -Recurse -Force }
    Expand-Archive -Path $Zip -DestinationPath $Destination -Force
    return $Destination
}

# Control API
$controlExe = Join-Path $InstallDir 'ts3stream-control.exe'
if ([string]::IsNullOrWhiteSpace($Version)) {
    $controlUrl = "https://github.com/$Repo/releases/latest/download/ts3stream-control.exe"
} else {
    $controlUrl = "https://github.com/$Repo/releases/download/$Version/ts3stream-control.exe"
}
Download-File $controlUrl $controlExe

# LiveKit
$livekitRelease = Invoke-RestMethod -Uri 'https://api.github.com/repos/livekit/livekit/releases/latest' -Headers @{ 'User-Agent' = 'TS3Stream' }
$livekitAsset = $livekitRelease.assets | Where-Object { $_.name -like '*windows_amd64.zip' } | Select-Object -First 1
if (-not $livekitAsset) { Fail 'Could not find a LiveKit Windows amd64 archive.' }
$livekitZip = Join-Path $env:TEMP 'livekit-windows.zip'
Download-File $livekitAsset.browser_download_url $livekitZip
$livekitExtract = Expand-Zip $livekitZip (Join-Path $env:TEMP 'livekit-windows')
$livekitExeSource = Get-ChildItem $livekitExtract -Recurse -Filter 'livekit-server.exe' | Select-Object -First 1
if (-not $livekitExeSource) { Fail 'livekit-server.exe not found in archive.' }
Copy-Item $livekitExeSource.FullName (Join-Path $InstallDir 'livekit-server.exe') -Force

# Caddy (optional)
$caddyExe = $null
if (-not $NoTls) {
    $caddyRelease = Invoke-RestMethod -Uri 'https://api.github.com/repos/caddyserver/caddy/releases/latest' -Headers @{ 'User-Agent' = 'TS3Stream' }
    $caddyAsset = $caddyRelease.assets | Where-Object { $_.name -like '*windows_amd64.zip' } | Select-Object -First 1
    if (-not $caddyAsset) { Fail 'Could not find a Caddy Windows amd64 archive.' }
    $caddyZip = Join-Path $env:TEMP 'caddy-windows.zip'
    Download-File $caddyAsset.browser_download_url $caddyZip
    $caddyExtract = Expand-Zip $caddyZip (Join-Path $env:TEMP 'caddy-windows')
    $caddySource = Get-ChildItem $caddyExtract -Recurse -Filter 'caddy.exe' | Select-Object -First 1
    if (-not $caddySource) { Fail 'caddy.exe not found in archive.' }
    $caddyExe = Join-Path $InstallDir 'caddy.exe'
    Copy-Item $caddySource.FullName $caddyExe -Force
}

# LiveKit config
$bindBlock = ''
$externalIp = 'false'
if (-not $NoTls) {
    $bindBlock = "bind_addresses:`r`n  - 127.0.0.1`r`n"
    $externalIp = 'true'
}
$livekitYaml = @"
port: 7880
$bindBlock
rtc:
  tcp_port: 7881
  udp_port: 7882
  use_external_ip: $externalIp

keys:
  ${ApiKey}: ${ApiSecret}

logging:
  level: info
"@
Set-Content -Path (Join-Path $InstallDir 'livekit.yaml') -Value $livekitYaml -Encoding ASCII

$controlAddr = if ($NoTls) { '0.0.0.0:8081' } else { '127.0.0.1:8081' }
$livekitUrl = if ($NoTls) { "ws://${Domain}:7880" } else { "wss://$Domain" }
$controlUrlPublic = if ($NoTls) { "http://${Domain}:8081" } else { "https://$Domain" }

# Wrapper scripts (environment and logs)
$startLiveKit = @"
@echo off
cd /d "$InstallDir"
"$InstallDir\livekit-server.exe" --config "$InstallDir\livekit.yaml" >> "$LogDir\livekit.log" 2>&1
"@
Set-Content -Path (Join-Path $InstallDir 'start-livekit.cmd') -Value $startLiveKit -Encoding ASCII

if (-not [string]::IsNullOrWhiteSpace($Ts3QueryPassword)) {
    $queryEnv = @"
set "TS3STREAM_WELCOME_AUTO=1"
set "TS3_QUERY_ADDR=127.0.0.1:10011"
set "TS3_QUERY_USER=serveradmin"
set "TS3_QUERY_PASSWORD=$Ts3QueryPassword"
set "TS3_QUERY_SID=1"
"@
} else {
    $queryEnv = 'set "TS3STREAM_WELCOME_AUTO=0"'
}

$startControl = @"
@echo off
cd /d "$InstallDir"
set "TS3STREAM_CONTROL_ADDR=$controlAddr"
set "TS3STREAM_CLIENT_KEY=$ClientKey"
set "TS3STREAM_PLAYOUT_DELAY_MS=$PlayoutDelayMs"
set "LIVEKIT_API_KEY=$ApiKey"
set "LIVEKIT_API_SECRET=$ApiSecret"
set "LIVEKIT_URL=$livekitUrl"
set "TS3STREAM_PUBLIC_URL=$controlUrlPublic"
$queryEnv
"$InstallDir\ts3stream-control.exe" >> "$LogDir\control.log" 2>&1
"@
Set-Content -Path (Join-Path $InstallDir 'start-control.cmd') -Value $startControl -Encoding ASCII

if (-not $NoTls) {
    $caddyfile = @"
$Domain {
    encode zstd gzip
    handle /api/* {
        reverse_proxy 127.0.0.1:8081
    }
    handle /rtc* {
        reverse_proxy 127.0.0.1:7880
    }
    handle /twirp/* {
        reverse_proxy 127.0.0.1:7880
    }
    handle {
        respond "TS3Stream" 200
    }
}
"@
    Set-Content -Path (Join-Path $InstallDir 'Caddyfile') -Value $caddyfile -Encoding ASCII
    $startCaddy = @"
@echo off
cd /d "$InstallDir"
"$InstallDir\caddy.exe" run --config "$InstallDir\Caddyfile" >> "$LogDir\caddy.log" 2>&1
"@
    Set-Content -Path (Join-Path $InstallDir 'start-caddy.cmd') -Value $startCaddy -Encoding ASCII
}

# Client config, also copied into the local TS3 client if present
$serviceJson = @"
{
  "controlUrl": "$controlUrlPublic",
  "clientKey": "$ClientKey"
}
"@
Set-Content -Path (Join-Path $InstallDir 'service.json') -Value $serviceJson -Encoding ASCII
$ts3Data = Join-Path $env:APPDATA 'TS3Client\plugins\ts3stream'
if (Test-Path (Join-Path $env:APPDATA 'TS3Client')) {
    New-Item -ItemType Directory -Force -Path $ts3Data | Out-Null
    Copy-Item (Join-Path $InstallDir 'service.json') (Join-Path $ts3Data 'service.json') -Force
    Write-Step "Copied service.json to $ts3Data"
}

# Restrict config files to Administrators and SYSTEM (SIDs are locale-independent)
$aclTargets = @(
    (Join-Path $InstallDir 'start-control.cmd'),
    (Join-Path $InstallDir 'livekit.yaml'),
    (Join-Path $InstallDir 'service.json')
)
foreach ($target in $aclTargets) {
    if (Test-Path $target) {
        & icacls.exe $target /inheritance:r /grant:r '*S-1-5-32-544:F' '*S-1-5-18:F' | Out-Null
    }
}

# Firewall
if (-not $SkipFirewall) {
    Write-Step 'Adding Windows Firewall rules'
    $rules = @(
        @{ Name = 'TS3Stream LiveKit TCP'; Protocol = 'TCP'; Ports = '7881' },
        @{ Name = 'TS3Stream LiveKit UDP'; Protocol = 'UDP'; Ports = '7882' }
    )
    if ($NoTls) {
        $rules += @{ Name = 'TS3Stream Control TCP'; Protocol = 'TCP'; Ports = '8081' }
        $rules += @{ Name = 'TS3Stream LiveKit Signal TCP'; Protocol = 'TCP'; Ports = '7880' }
    } else {
        $rules += @{ Name = 'TS3Stream HTTPS TCP'; Protocol = 'TCP'; Ports = '80,443' }
    }
    foreach ($rule in $rules) {
        Remove-NetFirewallRule -DisplayName $rule.Name -ErrorAction SilentlyContinue
        New-NetFirewallRule -DisplayName $rule.Name -Direction Inbound -Action Allow `
            -Protocol $rule.Protocol -LocalPort $rule.Ports | Out-Null
    }
}

# Scheduled tasks at startup
if (-not $NoServices) {
    Write-Step 'Registering startup tasks'
    $tasks = @(
        @{ Name = 'TS3Stream LiveKit'; Script = (Join-Path $InstallDir 'start-livekit.cmd') }
        @{ Name = 'TS3Stream Control'; Script = (Join-Path $InstallDir 'start-control.cmd') }
    )
    if (-not $NoTls) {
        $tasks += @{ Name = 'TS3Stream Caddy'; Script = (Join-Path $InstallDir 'start-caddy.cmd') }
    }
    foreach ($task in $tasks) {
        $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"$($task.Script)`""
        $trigger = New-ScheduledTaskTrigger -AtStartup
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
        $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
        Register-ScheduledTask -TaskName $task.Name -Action $action -Trigger $trigger `
            -Settings $settings -Principal $principal -Force | Out-Null
    }
}

# Start now
Write-Step 'Starting services'
Start-Process -FilePath (Join-Path $InstallDir 'start-livekit.cmd') -WindowStyle Hidden
Start-Sleep -Seconds 2
Start-Process -FilePath (Join-Path $InstallDir 'start-control.cmd') -WindowStyle Hidden
if (-not $NoTls) {
    Start-Sleep -Seconds 2
    Start-Process -FilePath (Join-Path $InstallDir 'start-caddy.cmd') -WindowStyle Hidden
}

# Health check
$healthUrl = if ($NoTls) { 'http://127.0.0.1:8081/api/health' } else { "https://$Domain/api/health" }
$healthy = $false
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Seconds 2
    try {
        $result = Invoke-RestMethod -Uri $healthUrl -TimeoutSec 5
        if ($result.status -eq 'ok') { $healthy = $true; break }
    } catch { }
}
if (-not $healthy) {
    Write-Warn "Health check did not pass: $healthUrl"
    Write-Warn "Check logs in $LogDir"
}

# TS3 check
$ts3 = Get-Process ts3server -ErrorAction SilentlyContinue
$ts3Udp = Get-NetUDPEndpoint -LocalPort 9987 -ErrorAction SilentlyContinue
if (-not $ts3 -and -not $ts3Udp) {
    Write-Warn 'TeamSpeak 3 server was not detected. It is installed separately and is not modified by this installer.'
}

Write-Host ''
Write-Host '=============================================================================='
Write-Host 'TS3Stream Windows server installation finished'
Write-Host '=============================================================================='
Write-Host "Install dir:  $InstallDir"
Write-Host "Control API:  $controlUrlPublic"
Write-Host "LiveKit URL:  $livekitUrl"
Write-Host "Client key:   $ClientKey"
Write-Host ''
Write-Host 'Client file (already copied if TS3 is local):'
Write-Host "  $InstallDir\service.json"
Write-Host '  %APPDATA%\TS3Client\plugins\ts3stream\service.json'
Write-Host ''
Write-Host $serviceJson
Write-Host 'Logs:'
Write-Host "  $LogDir"
Write-Host ''
Write-Host 'Open TS3 server ports separately: 9987/udp, 10011/tcp (optional), 30033/tcp (optional).'
Write-Host '=============================================================================='
