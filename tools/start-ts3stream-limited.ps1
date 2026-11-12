[CmdletBinding()]
param(
    [ValidateRange(1, 50)]
    [int]$MaxMbps = 4,
    [string]$TeamSpeakExe,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
if (-not $DryRun -and (Get-Process -Name 'ts3client_win64' -ErrorAction SilentlyContinue)) {
    throw 'Close TeamSpeak completely before running this launcher. Existing processes keep their old bitrate setting.'
}

if (-not $TeamSpeakExe) {
    $candidate = Join-Path $env:ProgramFiles 'TeamSpeak 3 Client\ts3client_win64.exe'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $TeamSpeakExe = $candidate
    } elseif ($DryRun) {
        throw 'Specify -TeamSpeakExe for a non-interactive dry run.'
    } else {
        Add-Type -AssemblyName System.Windows.Forms
        $picker = New-Object System.Windows.Forms.OpenFileDialog
        try {
            $picker.Title = 'Select TeamSpeak 3 (64-bit)'
            $picker.Filter = 'TeamSpeak 3|ts3client_win64.exe'
            if ($picker.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return }
            $TeamSpeakExe = $picker.FileName
        } finally {
            $picker.Dispose()
        }
    }
}

$file = Get-Item -LiteralPath $TeamSpeakExe
if ($file.PSIsContainer -or $file.Name -ine 'ts3client_win64.exe') {
    throw 'Select ts3client_win64.exe, not a shortcut or the installer.'
}
$bitrate = $MaxMbps * 1000000
if ($DryRun) {
    [pscustomobject]@{ Executable = $file.FullName; MaximumBitrate = $bitrate; WouldStart = $true }
    return
}

# Only this child process inherits the limit. No registry, service.json or
# persistent user/system environment setting is changed.
$previous = $env:TS3STREAM_MAX_BITRATE
try {
    $env:TS3STREAM_MAX_BITRATE = [string]$bitrate
    $process = Start-Process -FilePath $file.FullName -WorkingDirectory $file.DirectoryName -PassThru
    Write-Output "TeamSpeak started (PID $($process.Id)), diagnostic bitrate environment: $MaxMbps Mbit/s."
    Write-Output 'TS3Stream 0.3.10+: the bitrate selected in the stream dialog overrides this legacy environment setting.'
    Write-Output "After starting a stream, verify maxBitrate=$bitrate in the publisher's diag.log."
} finally {
    if ($null -eq $previous) {
        Remove-Item Env:TS3STREAM_MAX_BITRATE -ErrorAction SilentlyContinue
    } else {
        $env:TS3STREAM_MAX_BITRATE = $previous
    }
}
