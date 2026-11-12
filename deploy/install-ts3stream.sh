#!/usr/bin/env bash
#
# TS3Stream interactive installer for Debian/Ubuntu.
#
# It installs and configures:
#   - LiveKit SFU
#   - ts3stream-control API
#   - Caddy with automatic TLS (optional)
#
# It does NOT install or modify the TeamSpeak 3 server: install that separately.
#
# Examples:
#   sudo bash install-ts3stream.sh
#   sudo bash install-ts3stream.sh --domain stream.example.com --yes
#   sudo bash install-ts3stream.sh --domain 192.168.1.50 --no-caddy
#   sudo bash install-ts3stream.sh --domain stream.example.com --version v0.2.1
#
set -euo pipefail

REPO="${TS3STREAM_REPO:-ActiveDown/ts3stream}"
CONTROL_VERSION="${TS3STREAM_VERSION:-}"
PLAYOUT_DELAY_MS="${TS3STREAM_PLAYOUT_DELAY_MS:-2000}"
PLAYOUT_DELAY_SET=0
[[ -n "${TS3STREAM_PLAYOUT_DELAY_MS:-}" ]] && PLAYOUT_DELAY_SET=1
DOMAIN=""
API_KEY="ts3stream"
API_SECRET=""
CLIENT_KEY=""
INSTALL_CADDY=1
CONFIGURE_FIREWALL=1
ASSUME_YES=0
LIVEKIT_ARCHIVE=""

usage() {
    cat <<'EOF'
TS3Stream Linux installer

Usage:
  sudo bash install-ts3stream.sh [options]

Options:
  --domain NAME          Public DNS name or LAN IP (required in non-interactive mode)
  --api-key KEY          LiveKit API key (default: ts3stream)
  --api-secret SECRET    LiveKit API secret (default: generated)
  --client-key KEY       Shared key for the TS3 plugin (default: generated)
  --version TAG          Control API release tag, e.g. v0.2.1 (default: latest)
  --playout-delay-ms MS   A/V buffer: 0..5000, steps of 10 (default: 2000; plugin 0.3.8+)
  --no-caddy             Skip Caddy/TLS; use http:// and ws:// (LAN/testing)
  --no-firewall          Do not add UFW rules
  --livekit-archive URL  Override the LiveKit Linux amd64 archive URL
  -y, --yes              Do not ask for confirmation
  -h, --help             Show this help

After installation, create %APPDATA%\TS3Client\plugins\ts3stream\service.json
on each Windows client. The installer prints the exact JSON.
EOF
}

log()  { printf '\033[1;36m[ts3stream]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warning]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

need_root() {
    if [[ "${EUID}" -ne 0 ]]; then
        die "run this installer as root: sudo bash $0"
    fi
}

need_commands() {
    for cmd in curl tar openssl systemctl; do
        command -v "$cmd" >/dev/null 2>&1 || die "missing command: $cmd"
    done
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --domain) DOMAIN="${2:-}"; shift 2 ;;
            --api-key) API_KEY="${2:-}"; shift 2 ;;
            --api-secret) API_SECRET="${2:-}"; shift 2 ;;
            --client-key) CLIENT_KEY="${2:-}"; shift 2 ;;
            --version) CONTROL_VERSION="${2:-}"; shift 2 ;;
            --playout-delay-ms) PLAYOUT_DELAY_MS="${2:-}"; PLAYOUT_DELAY_SET=1; shift 2 ;;
            --livekit-archive) LIVEKIT_ARCHIVE="${2:-}"; shift 2 ;;
            --no-caddy) INSTALL_CADDY=0; shift ;;
            --no-firewall) CONFIGURE_FIREWALL=0; shift ;;
            -y|--yes) ASSUME_YES=1; shift ;;
            -h|--help) usage; exit 0 ;;
            *) die "unknown option: $1 (use --help)" ;;
        esac
    done
}

check_os() {
    if [[ ! -r /etc/os-release ]]; then
        die "cannot read /etc/os-release"
    fi
    # shellcheck disable=SC1091
    . /etc/os-release
    case "${ID:-}" in
        ubuntu|debian) ;;
        *) warn "tested on Ubuntu/Debian; detected: ${ID:-unknown}" ;;
    esac
    case "$(uname -m)" in
        x86_64|amd64) ;;
        *) die "only x86_64/amd64 is supported by the published control binary" ;;
    esac
}

ask_settings() {
    if [[ "$PLAYOUT_DELAY_SET" -eq 0 && -r /etc/ts3stream/control.env ]]; then
        local previous_delay
        previous_delay="$(grep '^TS3STREAM_PLAYOUT_DELAY_MS=' /etc/ts3stream/control.env | tail -n 1 || true)"
        PLAYOUT_DELAY_MS="${previous_delay#*=}"
        PLAYOUT_DELAY_MS="${PLAYOUT_DELAY_MS:-0}"
    fi
    [[ "$PLAYOUT_DELAY_MS" =~ ^[0-9]{1,4}$ ]] || die "invalid --playout-delay-ms"
    PLAYOUT_DELAY_MS=$((10#$PLAYOUT_DELAY_MS))
    (( PLAYOUT_DELAY_MS <= 5000 && PLAYOUT_DELAY_MS % 10 == 0 )) || die "playout delay must be 0..5000 in steps of 10 ms"
    log "A/V buffer: ${PLAYOUT_DELAY_MS} ms (requires viewer plugin 0.3.8+)"
    DOMAIN="${DOMAIN#http://}"
    DOMAIN="${DOMAIN#https://}"
    DOMAIN="${DOMAIN%/}"

    if [[ -z "${DOMAIN}" ]]; then
        if [[ -t 0 ]]; then
            read -r -p "Public DNS name or LAN IP (e.g. stream.example.com): " DOMAIN
        else
            die "--domain is required in non-interactive mode"
        fi
    fi
    [[ -n "${DOMAIN}" ]] || die "domain must not be empty"

    if [[ "${INSTALL_CADDY}" -eq 1 && "${DOMAIN}" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        local freeDns="${DOMAIN//./-}.sslip.io"
        die "Caddy needs a DNS name, not an IP. Re-run with --no-caddy for http/ws, or use a free DNS name such as ${freeDns} or FreeDNS (freedns.afraid.org) and open ports 80/443."
    fi

    if [[ -z "${API_SECRET}" ]]; then
        API_SECRET="$(openssl rand -hex 32)"
        log "generated LiveKit API secret"
    fi
    if [[ -z "${CLIENT_KEY}" ]]; then
        CLIENT_KEY="$(openssl rand -hex 24)"
        log "generated client shared key"
    fi

    if [[ "${ASSUME_YES}" -eq 0 && -t 0 ]]; then
        echo
        echo "Install plan:"
        echo "  domain:       ${DOMAIN}"
        echo "  TLS/Caddy:    $([[ ${INSTALL_CADDY} -eq 1 ]] && echo yes || echo no)"
        echo "  firewall:     $([[ ${CONFIGURE_FIREWALL} -eq 1 ]] && echo UFW rules || echo manual)"
        echo "  control tag:  ${CONTROL_VERSION:-latest}"
        echo
        read -r -p "Continue? [Y/n] " answer
        case "${answer}" in
            n|N|no|NO) die "cancelled" ;;
        esac
    fi
}

install_packages() {
    log "installing base packages"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -y
    apt-get install -y --no-install-recommends ca-certificates curl tar openssl gnupg

    if [[ "${INSTALL_CADDY}" -eq 1 ]] && ! command -v caddy >/dev/null 2>&1; then
        log "installing Caddy"
        apt-get install -y --no-install-recommends debian-keyring debian-archive-keyring apt-transport-https
        curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
            | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
        curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
            > /etc/apt/sources.list.d/caddy-stable.list
        apt-get update -y
        apt-get install -y --no-install-recommends caddy
    fi
}

create_users() {
    log "creating service users and directories"
    id -u ts3 >/dev/null 2>&1 || useradd --system --home /opt/ts3server --shell /usr/sbin/nologin ts3
    id -u livekit >/dev/null 2>&1 || useradd --system --home /nonexistent --shell /usr/sbin/nologin livekit
    id -u ts3stream >/dev/null 2>&1 || useradd --system --home /nonexistent --shell /usr/sbin/nologin ts3stream
    install -d -m 0755 /opt/ts3stream/bin /etc/ts3stream /var/log/ts3stream
}

stop_existing_services() {
    # A running executable cannot be opened for write (ETXTBSY). Stop services
    # and unmanaged processes before replacing binaries/config files.
    local unit
    for unit in ts3stream-control.service livekit.service caddy.service; do
        systemctl stop "$unit" >/dev/null 2>&1 || true
    done
    if command -v pkill >/dev/null 2>&1; then
        pkill -f '/opt/ts3stream/bin/ts3stream-control' >/dev/null 2>&1 || true
        pkill -f '/opt/ts3stream/bin/livekit-server' >/dev/null 2>&1 || true
    fi
}

download_control() {
    local url
    if [[ -n "${CONTROL_VERSION}" ]]; then
        url="https://github.com/${REPO}/releases/download/${CONTROL_VERSION}/ts3stream-control-linux-amd64"
    else
        url="https://github.com/${REPO}/releases/latest/download/ts3stream-control-linux-amd64"
    fi
    local target="/opt/ts3stream/bin/ts3stream-control"
    local temp="${target}.new"
    log "downloading control API: ${url}"
    rm -f "$temp"
    curl -fL --retry 3 -o "$temp" "${url}"
    chmod 0755 "$temp"
    # rename() replaces the directory entry even if the old binary is mapped.
    mv -f "$temp" "$target"
}

download_livekit() {
    local url="${LIVEKIT_ARCHIVE}"
    if [[ -z "${url}" ]]; then
        log "resolving latest LiveKit linux amd64 release"
        local api="https://api.github.com/repos/livekit/livekit/releases/latest"
        url="$(curl -fsSL "${api}" | grep -o 'https://[^"]*linux_amd64.tar.gz' | head -n 1 || true)"
        [[ -n "${url}" ]] || die "could not resolve LiveKit release; use --livekit-archive URL"
    fi
    log "downloading LiveKit: ${url}"
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp:-}"' EXIT
    curl -fL --retry 3 -o "${tmp}/livekit.tar.gz" "${url}"
    tar -xzf "${tmp}/livekit.tar.gz" -C "${tmp}"
    [[ -f "${tmp}/livekit-server" ]] || die "livekit-server binary not found in archive"
    local target="/opt/ts3stream/bin/livekit-server"
    local temp="${target}.new"
    install -m 0755 "${tmp}/livekit-server" "$temp"
    mv -f "$temp" "$target"
    rm -rf "${tmp}"
    trap - EXIT
}

write_configs() {
    log "writing LiveKit and control configuration"

    local bind_line=""
    if [[ "${INSTALL_CADDY}" -eq 1 ]]; then
        bind_line=$'bind_addresses:\n  - 127.0.0.1\n'
    fi

    cat > /etc/ts3stream/livekit.yaml <<EOF
port: 7880
${bind_line}
rtc:
  tcp_port: 7881
  udp_port: 7882
  use_external_ip: true

keys:
  ${API_KEY}: ${API_SECRET}

logging:
  level: info
EOF
    chown livekit:livekit /etc/ts3stream/livekit.yaml
    chmod 0640 /etc/ts3stream/livekit.yaml

    local control_addr="127.0.0.1:8081"
    local livekit_url="wss://${DOMAIN}"
    if [[ "${INSTALL_CADDY}" -eq 0 ]]; then
        control_addr="0.0.0.0:8081"
        livekit_url="ws://${DOMAIN}:7880"
    fi

    cat > /etc/ts3stream/control.env <<EOF
TS3STREAM_CONTROL_ADDR=${control_addr}
TS3STREAM_CLIENT_KEY=${CLIENT_KEY}
TS3STREAM_PLAYOUT_DELAY_MS=${PLAYOUT_DELAY_MS}
LIVEKIT_API_KEY=${API_KEY}
LIVEKIT_API_SECRET=${API_SECRET}
LIVEKIT_URL=${livekit_url}
EOF
    chown ts3stream:ts3stream /etc/ts3stream/control.env
    chmod 0640 /etc/ts3stream/control.env
}

write_services() {
    log "writing systemd services"

    cat > /etc/systemd/system/livekit.service <<'EOF'
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
EOF

    cat > /etc/systemd/system/ts3stream-control.service <<'EOF'
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
EOF

    systemctl daemon-reload
}

configure_caddy() {
    [[ "${INSTALL_CADDY}" -eq 1 ]] || return 0
    log "configuring Caddy for ${DOMAIN}"

    local caddyfile="/etc/caddy/Caddyfile"
    if [[ -f "${caddyfile}" ]]; then
        cp -a "${caddyfile}" "${caddyfile}.before-ts3stream"
    fi

    cat > "${caddyfile}" <<EOF
${DOMAIN} {
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
EOF

    if ! caddy validate --config "${caddyfile}" >/dev/null; then
        [[ -f "${caddyfile}.before-ts3stream" ]] && mv -f "${caddyfile}.before-ts3stream" "${caddyfile}"
        die "Caddyfile validation failed"
    fi
}

configure_firewall() {
    [[ "${CONFIGURE_FIREWALL}" -eq 1 ]] || return 0
    command -v ufw >/dev/null 2>&1 || { warn "ufw not found; configure firewall manually"; return 0; }
    log "adding UFW rules"
    ufw allow 80/tcp >/dev/null || true
    ufw allow 443/tcp >/dev/null || true
    ufw allow 7881/tcp >/dev/null || true
    ufw allow 7882/udp >/dev/null || true
    if [[ "${INSTALL_CADDY}" -eq 0 ]]; then
        ufw allow 8081/tcp >/dev/null || true
        ufw allow 7880/tcp >/dev/null || true
    fi
    if ufw status 2>/dev/null | grep -q '^Status: active'; then
        ufw reload >/dev/null || true
    else
        warn "UFW is inactive; rules were added but the firewall is not enabled"
    fi
}

start_services() {
    log "starting services"
    systemctl enable --now livekit.service
    systemctl enable --now ts3stream-control.service
    if [[ "${INSTALL_CADDY}" -eq 1 ]]; then
        systemctl enable caddy.service >/dev/null 2>&1 || true
        systemctl restart caddy.service
    fi
}

check_ts3() {
    if systemctl is-active --quiet ts3server.service 2>/dev/null; then
        log "ts3server.service is running"
        return
    fi
    if command -v ss >/dev/null 2>&1 && ss -lun 2>/dev/null | grep -q ':9987'; then
        log "TS3 voice port 9987/udp is listening"
        return
    fi
    warn "TS3 server was not detected. Install/start it separately; TS3Stream does not modify it."
}

wait_health() {
    local url
    if [[ "${INSTALL_CADDY}" -eq 1 ]]; then
        url="https://${DOMAIN}/api/health"
    else
        url="http://127.0.0.1:8081/api/health"
    fi
    log "waiting for ${url}"
    local i
    for i in $(seq 1 30); do
        if curl -fsS --max-time 5 "${url}" >/dev/null 2>&1; then
            log "control API is healthy"
            return 0
        fi
        sleep 2
    done
    warn "health check did not pass yet; inspect: journalctl -u ts3stream-control -n 80"
    return 1
}

print_result() {
    local control_url livekit_url
    if [[ "${INSTALL_CADDY}" -eq 1 ]]; then
        control_url="https://${DOMAIN}"
        livekit_url="wss://${DOMAIN}"
    else
        control_url="http://${DOMAIN}:8081"
        livekit_url="ws://${DOMAIN}:7880"
    fi

    cat <<EOF

==============================================================================
TS3Stream server installation finished
==============================================================================

Control API:  ${control_url}
LiveKit URL:  ${livekit_url}
Client key:   ${CLIENT_KEY}

Windows client file:
  %APPDATA%\\TS3Client\\plugins\\ts3stream\\service.json

{
  "controlUrl": "${control_url}",
  "clientKey": "${CLIENT_KEY}"
}

Services:
  systemctl status livekit
  systemctl status ts3stream-control
  $([[ ${INSTALL_CADDY} -eq 1 ]] && echo "systemctl status caddy")

Ports that must be reachable by clients:
  7881/tcp, 7882/udp
  $([[ ${INSTALL_CADDY} -eq 1 ]] && echo "443/tcp (Caddy / WSS / HTTPS)" || echo "8081/tcp, 7880/tcp (no TLS mode)")

TS3 server is separate and was not modified. Open its ports:
  9987/udp, 10011/tcp (optional), 30033/tcp (optional)
==============================================================================
EOF
}

main() {
    parse_args "$@"
    need_root
    need_commands
    check_os
    ask_settings
    install_packages
    create_users
    stop_existing_services
    download_control
    download_livekit
    write_configs
    write_services
    configure_caddy
    configure_firewall
    start_services
    check_ts3
    wait_health || true
    print_result
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then main "$@"; fi
