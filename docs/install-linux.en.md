# Installing TS3Stream on a Linux server

This guide installs a complete server stack on Ubuntu 22.04/24.04:

- stock TeamSpeak 3 server (not patched);
- LiveKit SFU;
- `ts3stream-control` API;
- Caddy for HTTPS/WSS.

The Windows client plugin is installed separately; see the end of this guide.

## Quick install (recommended)

On a fresh Ubuntu 22.04/24.04 or Debian 12 x86_64 host with the **stock TS3
server already installed**, run:

```bash
curl -fsSL https://raw.githubusercontent.com/ActiveDown/ts3stream/main/deploy/install-linux.sh -o install-linux.sh
less install-linux.sh
sudo bash install-linux.sh
```

Or one line (download to `/tmp`, then execute — this works with `sudo`):

```bash
curl -fsSL https://raw.githubusercontent.com/ActiveDown/ts3stream/main/deploy/install-linux.sh -o /tmp/install-linux.sh && sudo bash /tmp/install-linux.sh
```

The installer asks for the public DNS name and confirmation, then installs
LiveKit, the control API and Caddy, creates systemd services, generates secrets,
adds UFW rules and prints the exact `service.json` for Windows clients.

If the ServerQuery password is not found in the TS3 logs, the installer asks
for it and offers an automatic reset (the TS3 server is restarted). For
non-interactive installs use `--ts3-query-password '...'` or
`--reset-ts3-query`.

Non-interactive examples:

```bash
# Internet-facing with automatic TLS
sudo bash install-linux.sh --domain stream.example.com --yes

# LAN / test without TLS
sudo bash install-linux.sh --domain 192.168.1.50 --no-caddy --yes
```

Only a public IP, no domain:

```bash
# HTTP/WS, no TLS
sudo bash install-linux.sh --domain 95.142.44.214 --no-caddy --yes

# Free DNS + TLS: sslip.io resolves 95-142-44-214.sslip.io to 95.142.44.214
sudo bash install-linux.sh --domain 95-142-44-214.sslip.io --yes
```

`sslip.io` is a free wildcard DNS service; ports 80/443 must be reachable for
Caddy to obtain a Let's Encrypt certificate. You can also use DuckDNS or any
other DNS name you control.

FreeDNS (<https://freedns.afraid.org/>) works as well:

1. Register a free account and create a subdomain on one of the shared domains.
2. Add an **A record** with destination `95.142.44.214` (your public IP).
3. Verify: `dig +short yourname.mooo.com` — it must return the IP.
4. Run the installer with `--domain yourname.mooo.com`.

A single certificate for one subdomain is fine. Shared FreeDNS domains are
subject to normal Let's Encrypt rate limits, and there must be no proxy/CDN in
front of the A record unless it also forwards WebSocket and UDP media.

If you prefer full control or use another distribution, follow the manual steps
below.

## Manual installation

## 1. Prepare the host

You need:

- a Linux VPS with a public IPv4 address;
- a DNS name, for example `stream.example.com`, pointing to the VPS;
- root or sudo access;
- ports 80/443 reachable for Let's Encrypt.

Create service users and directories:

```bash
sudo useradd --system --home /opt/ts3server --shell /usr/sbin/nologin ts3 || true
sudo useradd --system --home /opt/ts3stream --shell /usr/sbin/nologin livekit || true
sudo useradd --system --home /opt/ts3stream --shell /usr/sbin/nologin ts3stream || true
sudo mkdir -p /opt/ts3server /opt/ts3stream/bin /etc/ts3stream /var/log/ts3stream
```

## 2. Install the stock TeamSpeak 3 server

```bash
cd /tmp
curl -LO https://files.teamspeak-services.com/releases/server/3.13.9/teamspeak3-server_linux_amd64-3.13.9.tar.bz2
tar -xjf teamspeak3-server_linux_amd64-3.13.9.tar.bz2
sudo cp -a teamspeak3-server_linux_amd64/. /opt/ts3server/
sudo chown -R ts3:ts3 /opt/ts3server
```

Create `/etc/systemd/system/ts3server.service`:

```ini
[Unit]
Description=TeamSpeak 3 Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=ts3
Group=ts3
WorkingDirectory=/opt/ts3server
ExecStart=/opt/ts3server/ts3server license_accepted=1
Restart=on-failure
RestartSec=3
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

Start it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ts3server
```

On the first start TeamSpeak prints a **ServerAdmin privilege key** and a
ServerQuery password. Copy them:

```bash
sudo journalctl -u ts3server -n 100 --no-pager
# or:
sudo grep -R "token=" /opt/ts3server/logs | tail
```

You do not need the ServerQuery account for TS3Stream, but keep the privilege
key: it is the only way to become server admin in the TS3 client.

## 3. Install LiveKit

Download the latest Linux amd64 archive from
<https://github.com/livekit/livekit/releases> and extract the `livekit-server`
binary. Example for v1.13.11:

```bash
cd /tmp
curl -L -o livekit.tar.gz \
  https://github.com/livekit/livekit/releases/download/v1.13.11/livekit_1.13.11_linux_amd64.tar.gz
tar -xzf livekit.tar.gz
sudo install -m 0755 livekit-server /opt/ts3stream/bin/livekit-server
```

Generate a strong API secret:

```bash
openssl rand -hex 32
```

Create `/etc/ts3stream/livekit.yaml` (replace `PASTE_SECRET`):

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

logging:
  level: info
```

Notes:

- `7880` is signaling and stays on localhost behind Caddy.
- `7881/TCP` and `7882/UDP` carry media and must be reachable from clients.
- TURN is optional and disabled in this minimal config. See section 8.

Create `/etc/systemd/system/livekit.service`:

```ini
[Unit]
Description=LiveKit Server for TS3Stream
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=livekit
Group=livekit
ExecStart=/opt/ts3stream/bin/livekit-server --config /etc/ts3stream/livekit.yaml
Restart=on-failure
RestartSec=3
LimitNOFILE=65536
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

```bash
sudo chown -R livekit:livekit /etc/ts3stream/livekit.yaml
sudo systemctl daemon-reload
sudo systemctl enable --now livekit
```

## 4. Install the control API

Download `ts3stream-control-linux-amd64` from the TS3Stream release page:

```bash
sudo curl -L -o /opt/ts3stream/bin/ts3stream-control \
  https://github.com/ActiveDown/ts3stream/releases/download/v0.3.1/ts3stream-control-linux-amd64
sudo chmod +x /opt/ts3stream/bin/ts3stream-control
```

Create `/etc/ts3stream/control.env` (replace both secrets):

```bash
TS3STREAM_CONTROL_ADDR=127.0.0.1:8081
TS3STREAM_CLIENT_KEY=PASTE_CLIENT_KEY
LIVEKIT_API_KEY=ts3stream
LIVEKIT_API_SECRET=PASTE_SECRET
LIVEKIT_URL=wss://stream.example.com

# Auto-configuration / UID verification (installer fills these automatically)
TS3STREAM_PUBLIC_URL=https://stream.example.com
TS3STREAM_WELCOME_AUTO=1
TS3_QUERY_ADDR=127.0.0.1:10011
TS3_QUERY_USER=serveradmin
TS3_QUERY_PASSWORD=PASTE_QUERY_PASSWORD
TS3_QUERY_SID=1
```

`LIVEKIT_URL` is returned to the Windows plugin, so it must be the public WSS
address, not `127.0.0.1`.

The `TS3_QUERY_*` block enables `/api/register` and the welcome message marker.
The installer detects the ServerQuery password from the first-run TS3 logs. If
you set it up manually and the password is missing, clients can still use a
manual `service.json`. Control API logs the exact query error on startup.

Create `/etc/systemd/system/ts3stream-control.service`:

```ini
[Unit]
Description=TS3Stream control API
After=network-online.target livekit.service
Wants=network-online.target

[Service]
Type=simple
User=ts3stream
Group=ts3stream
EnvironmentFile=/etc/ts3stream/control.env
ExecStart=/opt/ts3stream/bin/ts3stream-control
Restart=on-failure
RestartSec=3
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

```bash
sudo chown -R ts3stream:ts3stream /etc/ts3stream/control.env /var/log/ts3stream
sudo systemctl daemon-reload
sudo systemctl enable --now ts3stream-control
```

## 5. Install Caddy and TLS

```bash
sudo apt install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
  | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
  | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt update
sudo apt install -y caddy
```

Replace `/etc/caddy/Caddyfile` with:

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

```bash
sudo caddy validate --config /etc/caddy/Caddyfile
sudo systemctl restart caddy
curl https://stream.example.com/api/health
# {"status":"ok"}
```

Caddy obtains and renews the TLS certificate automatically.

## 6. Firewall

With UFW:

```bash
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw allow 9987/udp
sudo ufw allow 10011/tcp
sudo ufw allow 30033/tcp
sudo ufw allow 7881/tcp
sudo ufw allow 7882/udp
# only if you enable TURN:
sudo ufw allow 3478/udp
sudo ufw allow 5349/tcp
sudo ufw enable
```

Do **not** expose `7880` or `8081` directly; Caddy proxies them on localhost.

## 7. Install and configure the Windows client

1. Install `ts3stream-0.3.1.ts3_plugin` with the TeamSpeak Package Installer.
2. Start TS3, connect to your server.

Normally no manual file is needed. The plugin reads the
`[ts3stream]https://...[/ts3stream]` marker from the TS3 welcome message, calls
`/api/register`, verifies the TS3 UID through ServerQuery and saves
`service.json` automatically.

If auto-configuration is disabled (the installer did not find the ServerQuery
password), create `%APPDATA%\TS3Client\plugins\ts3stream\service.json` manually:

```json
{
  "controlUrl": "https://stream.example.com",
  "clientKey": "PASTE_CLIENT_KEY"
}
```

Then restart TS3. The **Stream** button becomes active only while connected.
Start a stream, then watch it from the client context menu, the client info
button, or the LIVE badge.

## 8. Optional: TURN/TLS

TURN helps clients behind restrictive firewalls.

1. Point `turn.example.com` to the same host.
2. Obtain a certificate with certbot:

```bash
sudo apt install -y certbot
sudo systemctl stop caddy
sudo certbot certonly --standalone -d turn.example.com
sudo systemctl start caddy
```

3. Copy the certificate where the LiveKit user can read it, then add TURN to
   `/etc/ts3stream/livekit.yaml`:

```bash
sudo mkdir -p /etc/ts3stream/certs
sudo cp /etc/letsencrypt/live/turn.example.com/fullchain.pem /etc/ts3stream/certs/
sudo cp /etc/letsencrypt/live/turn.example.com/privkey.pem /etc/ts3stream/certs/
sudo chown -R livekit:livekit /etc/ts3stream/certs
sudo chmod 640 /etc/ts3stream/certs/*
```

```yaml
turn:
  enabled: true
  domain: turn.example.com
  tls_port: 5349
  udp_port: 3478
  cert_file: /etc/ts3stream/certs/fullchain.pem
  key_file: /etc/ts3stream/certs/privkey.pem
```

4. Restart LiveKit:

```bash
sudo systemctl restart livekit
```

5. Open `3478/udp` and `5349/tcp` in the firewall.

6. After each certificate renewal, copy the new `fullchain.pem` and
   `privkey.pem` into `/etc/ts3stream/certs/` and restart LiveKit. A certbot
   deploy hook can automate this.

## 9. Verification

```bash
sudo systemctl status ts3server livekit ts3stream-control caddy
curl https://stream.example.com/api/health
curl https://stream.example.com/api/streams
sudo journalctl -u ts3stream-control -n 50 --no-pager
```

In the TS3 client:

- connect to `stream.example.com:9987` (or the server IP);
- press **Stream**;
- the LIVE badge must appear for other clients in the channel;
- the in-client viewer must receive 1080p video.

## 10. Upgrades

```bash
sudo systemctl stop ts3stream-control
sudo curl -L -o /opt/ts3stream/bin/ts3stream-control \
  https://github.com/ActiveDown/ts3stream/releases/latest/download/ts3stream-control-linux-amd64
sudo chmod +x /opt/ts3stream/bin/ts3stream-control
sudo systemctl start ts3stream-control
```

Replace the plugin DLL on Windows with the new `.ts3_plugin` package.

## 11. Troubleshooting

- **401 from control API**: `clientKey` in `service.json` does not match
  `TS3STREAM_CLIENT_KEY`.
- **Token issued, no video**: open `7881/TCP` and `7882/UDP`; check
  `use_external_ip` and the host firewall.
- **No LIVE badge**: check `curl https://.../api/streams` and the control logs;
  the registry TTL is 15 seconds.
- **Caddy certificate error**: DNS must point to the host and port 80 must be
  reachable.
- **LiveKit does not start**: validate the YAML and make sure the API secret
  matches `control.env`.




