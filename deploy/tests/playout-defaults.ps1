$ErrorActionPreference = 'Stop'
$path = Join-Path $PSScriptRoot '..\install-ts3stream.ps1'
$tokens = $null
$errors = $null
[Management.Automation.Language.Parser]::ParseFile((Resolve-Path $path), [ref]$tokens, [ref]$errors) | Out-Null
if ($errors) { throw ($errors | Out-String) }
# Execute only the installer's actual parameter/config-selection preamble.
# No elevation check, download, service, firewall or installation code runs.
$source = [IO.File]::ReadAllText((Resolve-Path $path))
$end = $source.IndexOf('function Write-Step(')
if ($end -lt 0) { throw 'Installer preamble boundary missing' }
$select = [scriptblock]::Create($source.Substring(0, $end) + "`nreturn `$PlayoutDelayMs")
$selectMode = [scriptblock]::Create($source.Substring(0, $end) + "`nreturn `$BufferMode")
$temp = Join-Path ([IO.Path]::GetTempPath()) ('ts3stream-playout-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    if ((& $select -InstallDir $temp) -ne 2000) { throw 'Fresh install default is not 2000' }
    if ((& $selectMode -InstallDir $temp) -ne 'server') { throw 'Fresh install must use server buffering' }
    Set-Content (Join-Path $temp 'start-control.cmd') '@echo off'
    if ((& $selectMode -InstallDir $temp) -ne 'client') { throw 'Legacy mode not preserved' }
    Set-Content (Join-Path $temp 'start-control.cmd') 'set "TS3STREAM_BUFFER_MODE=server"'
    if ((& $selectMode -InstallDir $temp) -ne 'server') { throw 'Server mode not preserved' }
    if ((& $selectMode -InstallDir $temp -BufferMode client) -ne 'client') { throw 'Mode override ignored' }
    if ((& $select -InstallDir $temp) -ne 0) { throw 'Legacy install must preserve zero delay' }
    Set-Content (Join-Path $temp 'start-control.cmd') 'set "TS3STREAM_PLAYOUT_DELAY_MS=3000"'
    if ((& $select -InstallDir $temp) -ne 3000) { throw 'Existing setting not preserved' }
    if ((& $select -InstallDir $temp -PlayoutDelayMs 2000) -ne 2000) { throw 'Explicit override lost' }
    if ((& $select -InstallDir $temp -PlayoutDelayMs 0) -ne 0) { throw 'Explicit disable lost' }
    $rejected = $false
    try { & $select -InstallDir $temp -PlayoutDelayMs 2001 | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Nonrepresentable RTP delay accepted' }
    'Windows installer fresh/default, preserved and explicit A/V delay: PASS'
} finally {
    Remove-Item $temp -Recurse -Force
}
