#!/usr/bin/env bash
set -euo pipefail

# Installs TS3Stream server-side components. Run as root.
# Expected files next to this script:
#   ts3stream-control        (linux/amd64)
#   livekit-server           (TS3Stream LiveKit build from the matching release)
#   livekit.yaml.example
#   control.env.example
#   livekit.service
#   ts3stream-control.service
#   Caddyfile.example

ROOT="$(cd "$(dirname "$0")" && pwd)"
install -d -m 0755 /opt/ts3stream/bin /etc/ts3stream /var/log/ts3stream
install -m 0755 "$ROOT/ts3stream-control" /opt/ts3stream/bin/ts3stream-control
install -m 0755 "$ROOT/livekit-server" /opt/ts3stream/bin/livekit-server

if [ ! -f /etc/ts3stream/livekit.yaml ]; then
    install -m 0640 "$ROOT/livekit.yaml.example" /etc/ts3stream/livekit.yaml
fi
if [ ! -f /etc/ts3stream/control.env ]; then
    install -m 0640 "$ROOT/control.env.example" /etc/ts3stream/control.env
fi

useradd --system --no-create-home --shell /usr/sbin/nologin livekit 2>/dev/null || true
useradd --system --no-create-home --shell /usr/sbin/nologin ts3stream 2>/dev/null || true
chown -R livekit:livekit /etc/ts3stream/livekit.yaml
chown -R ts3stream:ts3stream /etc/ts3stream/control.env /var/log/ts3stream

install -m 0644 "$ROOT/livekit.service" /etc/systemd/system/livekit.service
install -m 0644 "$ROOT/ts3stream-control.service" /etc/systemd/system/ts3stream-control.service
systemctl daemon-reload
systemctl enable --now livekit.service ts3stream-control.service

echo "Edit /etc/ts3stream/livekit.yaml and /etc/ts3stream/control.env, then restart both services."
echo "Install Caddyfile.example as your site config and obtain TLS certificates."
