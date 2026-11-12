# Installing TS3Stream on a Windows server

This guide is for a Windows machine that runs the TeamSpeak 3 server and the
TS3Stream server components. There are two common setups:

- **LAN / test**: `http://` control API and `ws://` LiveKit, no TLS. Easiest.
- **Internet-facing**: Caddy provides HTTPS/WSS. Use this for real users.

The Windows TS3 client plugin is installed separately; see section 7.

## Quick install (recommended)

Run PowerShell **as Administrator** and execute:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "iwr https://raw.githubusercontent.com/ActiveDown/ts3stream/main/deploy/install-ts3stream.ps1 -OutFile $env:TEMP\install-ts3stream.ps1; & $env:TEMP\install-ts3stream.ps1"
```

It installs LiveKit, the control API and optionally Caddy with TLS, writes all
configuration, adds firewall rules, creates startup tasks and checks the control
API health endpoint. The stock TS3 server is **not** installed or modified; the
installer only checks that it is running.

LAN/IP without TLS:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "iwr https://raw.githubusercontent.com/ActiveDown/ts3stream/main/deploy/install-ts3stream.ps1 -OutFile $env:TEMP\install-ts3stream.ps1; & $env:TEMP\install-ts3stream.ps1 -Domain 192.168.1.50 -NoTls -Yes"
```

Options: `-Domain`, `-ApiKey`, `-ApiSecret`, `-ClientKey`, `-Version`, `-NoTls`,
`-NoServices`, `-SkipFirewall`, `-Yes`.

The manual steps below remain available if you want full control.

## 1. Prepare directories

Open PowerShell as Administrator:

```powershell
New-Item -ItemType Directory -Force C:\TS3Server, C:\TS3Stream, C:\TS3Stream\logs | Out-Null
```

You need:

- a static LAN IP or a public IP/DNS name;
- open firewall ports;
- the latest TS3Stream release assets from
  <https://github.com/ActiveDown/ts3stream/releases>.

## 2. Install the stock TeamSpeak 3 server

1. Download `teamspeak3-server_win64-3.13.9.zip` from
   <https://teamspeak.com/en/downloads/#server>.
2. Extract it to `C:\TS3Server`.
3. Start it once from PowerShell:

```powershell
cd C:\TS3Server
.\ts3server.exe license_accepted=1
```

On first start it prints a **ServerAdmin privilege key**. Copy it. Keep the
window open for the first test, or install it as a service (section 9).

Default TS3 ports:

- `9987/UDP` — voice;
- `10011/TCP` — ServerQuery;
- `30033/TCP` — file transfer.

You do not need ServerQuery for TS3Stream, but the privilege key is required to
become server admin in the client.

## 3. Install LiveKit for Windows

1. Download the Windows amd64 archive `livekit_<version>_windows_amd64.zip` from
   <https://github.com/livekit/livekit/releases>.
2. Extract `livekit-server.exe` and put it in `C:\TS3Stream`.
3. Generate an API secret:

```powershell
-join ((48..57) + (97..102) | Get-Random -Count 64 | ForEach-Object {[char]$_})
```

4. Create `C:\TS3Stream\livekit.yaml`:

```yaml
port: 7880

rtc:
  tcp_port: 7881
  udp_port: 7882
  use_external_ip: false

keys:
  ts3stream: PASTE_SECRET

logging:
  level: info
```

For LAN use, do **not** set `bind_addresses` to `127.0.0.1`: clients must reach
`ws://SERVER_IP:7880` directly. For the TLS setup behind Caddy, see section 8.

5. Start it:

```powershell
cd C:\TS3Stream
.\livekit-server.exe --config .\livekit.yaml
```

Ports `7880/TCP`, `7881/TCP` and `7882/UDP` must be reachable by clients.

## 4. Install the control API

1. Download `ts3stream-control.exe` from the TS3Stream release.
2. Put it in `C:\TS3Stream`.
3. Start it with environment variables (replace `SERVER_IP`, secrets and keys):

```powershell
cd C:\TS3Stream
$env:TS3STREAM_CONTROL_ADDR = "0.0.0.0:8081"
$env:TS3STREAM_CLIENT_KEY = "PASTE_CLIENT_KEY"
$env:LIVEKIT_API_KEY = "ts3stream"
$env:LIVEKIT_API_SECRET = "PASTE_SECRET"
$env:LIVEKIT_URL = "ws://SERVER_IP:7880"
$env:TS3STREAM_PUBLIC_URL = "http://SERVER_IP:8081"
$env:TS3STREAM_WELCOME_AUTO = "1"
$env:TS3_QUERY_ADDR = "127.0.0.1:10011"
$env:TS3_QUERY_USER = "serveradmin"
$env:TS3_QUERY_PASSWORD = "PASTE_QUERY_PASSWORD"
$env:TS3_QUERY_SID = "1"
.\ts3stream-control.exe
```

`LIVEKIT_URL` is returned to every TS3 client, so it must be the address clients
can actually reach. `127.0.0.1` only works when the client is on the same PC.

The `TS3_QUERY_*` block enables UID-verified registration and the welcome marker.
The installer detects the password from `C:\TS3Server\logs`; if it was not found,
pass `-Ts3QueryPassword` or use a manual `service.json`.

4. Check:

```powershell
Invoke-RestMethod http://127.0.0.1:8081/api/health
# status : ok
```

## 5. Firewall

Run in an elevated PowerShell:

```powershell
New-NetFirewallRule -DisplayName "TS3 Voice UDP" -Direction Inbound -Protocol UDP -LocalPort 9987 -Action Allow
New-NetFirewallRule -DisplayName "TS3 Query TCP" -Direction Inbound -Protocol TCP -LocalPort 10011 -Action Allow
New-NetFirewallRule -DisplayName "TS3 Files TCP" -Direction Inbound -Protocol TCP -LocalPort 30033 -Action Allow
New-NetFirewallRule -DisplayName "TS3Stream LiveKit TCP" -Direction Inbound -Protocol TCP -LocalPort 7880,7881 -Action Allow
New-NetFirewallRule -DisplayName "TS3Stream LiveKit UDP" -Direction Inbound -Protocol UDP -LocalPort 7882 -Action Allow
New-NetFirewallRule -DisplayName "TS3Stream Control TCP" -Direction Inbound -Protocol TCP -LocalPort 8081 -Action Allow
```

For internet-facing production, do not expose `8081` directly; use Caddy and
open only `80/443` plus the TS3/LiveKit media ports.

## 6. Test from the server

```powershell
Invoke-RestMethod http://127.0.0.1:8081/api/health
Invoke-RestMethod http://127.0.0.1:8081/api/streams
Get-NetUDPEndpoint -LocalPort 9987
Test-NetConnection 127.0.0.1 -Port 7880
```

## 7. Install and configure the Windows client plugin

1. Close TeamSpeak 3.
2. Install `ts3stream-0.3.0.ts3_plugin` with the TeamSpeak Package Installer.
3. Start TS3 and connect to `SERVER_IP:9987`.

Normally no manual file is needed. The plugin reads the welcome marker, calls
`/api/register`, verifies the TS3 UID through ServerQuery and saves
`service.json` automatically.

If `TS3_QUERY_PASSWORD` was not configured, create
`%APPDATA%\TS3Client\plugins\ts3stream\service.json` manually:

```json
{
  "controlUrl": "http://SERVER_IP:8081",
  "clientKey": "PASTE_CLIENT_KEY"
}
```

Then restart TS3. Press **Stream**; the button is active only while connected.
Watch a stream from the client context menu, the client info button, or the
LIVE badge. Starting your own stream does not open the viewer.

## 8. Internet-facing TLS with Caddy (recommended for production)

A public TLS certificate requires a DNS name, not a bare IP. If you do not own a
domain, use FreeDNS (<https://freedns.afraid.org/>) or `sslip.io`:
`95-142-44-214.sslip.io` (dots become dashes). Create an A record pointing to
your public IP and verify it with `dig +short yourname.mooo.com` before running
Caddy.

1. Point DNS `stream.example.com` to the public IP.
2. Download `caddy.exe` for Windows from <https://caddyserver.com/download> and
   put it in `C:\TS3Stream`.
3. Create `C:\TS3Stream\Caddyfile`:

```caddyfile
stream.example.com {
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
```

4. Change LiveKit to listen on localhost only and restart it:

```yaml
port: 7880
bind_addresses:
  - 127.0.0.1
rtc:
  tcp_port: 7881
  udp_port: 7882
  use_external_ip: true
keys:
  ts3stream: PASTE_SECRET
```

5. Set control API:

```powershell
$env:TS3STREAM_CONTROL_ADDR = "127.0.0.1:8081"
$env:LIVEKIT_URL = "wss://stream.example.com"
```

6. Start Caddy:

```powershell
cd C:\TS3Stream
.\caddy.exe run --config .\Caddyfile
```

7. Open `80/TCP` and `443/TCP`; close `8081/TCP` from the internet.
8. In `service.json` use:

```json
{
  "controlUrl": "https://stream.example.com",
  "clientKey": "PASTE_CLIENT_KEY"
}
```

Caddy obtains the certificate automatically on Windows too.

## 9. Run as Windows services (optional)

Use [NSSM](https://nssm.cc/) so the server survives logoff/reboot:

```powershell
nssm install TS3Server "C:\TS3Server\ts3server.exe" "license_accepted=1"
nssm set TS3Server AppDirectory "C:\TS3Server"

nssm install TS3StreamLiveKit "C:\TS3Stream\livekit-server.exe" "--config C:\TS3Stream\livekit.yaml"
nssm set TS3StreamLiveKit AppDirectory "C:\TS3Stream"

nssm install TS3StreamControl "C:\TS3Stream\ts3stream-control.exe"
nssm set TS3StreamControl AppDirectory "C:\TS3Stream"
# Set the same environment variables in the service, for example:
# nssm set TS3StreamControl AppEnvironmentExtra LIVEKIT_API_KEY=ts3stream LIVEKIT_API_SECRET=... LIVEKIT_URL=ws://SERVER_IP:7880 TS3STREAM_CONTROL_ADDR=0.0.0.0:8081 TS3STREAM_CLIENT_KEY=...

nssm start TS3Server
nssm start TS3StreamLiveKit
nssm start TS3StreamControl
```

## 10. Optional TURN/TLS

If clients are behind strict firewalls, enable TURN in `livekit.yaml` with a
certificate for `turn.example.com`:

```yaml
turn:
  enabled: true
  domain: turn.example.com
  tls_port: 5349
  udp_port: 3478
  cert_file: C:\TS3Stream\certs\fullchain.pem
  key_file: C:\TS3Stream\certs\privkey.pem
```

Open `3478/UDP` and `5349/TCP`. On Windows the certificate must be a PEM file;
Let's Encrypt certificates can be copied from another host or obtained with
win-acme/certbot in WSL.

## 11. Troubleshooting

- **`/api/health` works locally, client gets 401**: `clientKey` and
  `TS3STREAM_CLIENT_KEY` differ.
- **Client gets a token but no video**: check `7881/TCP` and `7882/UDP`; if the
  client is remote, `LIVEKIT_URL` must not be `127.0.0.1`.
- **No LIVE badge**: `GET /api/streams` must show the stream; TTL is 15 seconds.
- **LiveKit fails to start**: YAML syntax, API key/secret mismatch, port already
  in use.
- **Windows firewall prompts**: allow private and public networks only as
  intended; for production, restrict `10011` and `8081` to trusted IPs.
