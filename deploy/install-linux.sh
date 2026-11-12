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
#   sudo bash install-linux.sh
#   sudo bash install-linux.sh --domain stream.example.com --yes
#   sudo bash install-linux.sh --domain 192.168.1.50 --no-caddy
#   sudo bash install-linux.sh --domain stream.example.com --version v0.3.0
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
TS3_QUERY_PASSWORD="${TS3_QUERY_PASSWORD:-}"
TS3_QUERY_PORT="${TS3_QUERY_PORT:-10011}"
TS3_QUERY_PORT_SET=0
TS3_SERVER_DIR="${TS3_SERVER_DIR:-}"
RESET_TS3_QUERY=0
INSTALL_CADDY=1
CONFIGURE_FIREWALL=1
ASSUME_YES=0
SKIP_DNS_CHECK=0
DNS_WARNING=""
DDNS_UPDATE_URL="${TS3STREAM_DDNS_UPDATE_URL:-}"
LIVEKIT_ARCHIVE=""

usage() {
    cat <<'EOF'
TS3Stream Linux installer

Usage:
  sudo bash install-linux.sh [options]

Options:
  --domain NAME          Public DNS name or LAN IP (required in non-interactive mode)
  --api-key KEY          LiveKit API key (default: ts3stream)
  --api-secret SECRET    LiveKit API secret (default: generated)
  --client-key KEY       Shared key for the TS3 plugin (default: generated)
  --ts3-query-password P  ServerQuery password; auto-detected from TS3 logs by default
  --ts3-query-port PORT  ServerQuery port (default: 10011 or the value from ts3server.ini)
  --ts3-dir DIR          TeamSpeak 3 server directory (default: auto-detect)
  --reset-ts3-query      If the password is unknown, reset it automatically (server restart)
  --version TAG          Control API release tag, e.g. v0.3.1 (default: latest)
  --playout-delay-ms MS   A/V buffer: 0..5000, steps of 10 (new installs: 2000)
                         Requires viewer plugin 0.3.8+; existing setting is preserved
  --no-caddy             Skip Caddy/TLS; use http:// and ws:// (LAN/testing)
  --no-firewall          Do not add UFW rules
  --skip-dns-check       Do not verify that the domain points to this server
  --ddns-update-url URL  Call a FreeDNS/no-IP update URL before the DNS check
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

is_no() {
    case "${1:-}" in
        n|N|no|NO|No|nO|н|Н|нет|Нет|НЕТ|no.|No.|nope) return 0 ;;
    esac
    return 1
}

read_env_value() {
    local key="$1" file="$2" line value
    [[ -r "$file" ]] || return 0
    line="$(grep -E "^[[:space:]]*${key}=" "$file" | tail -n 1 || true)"
    [[ -n "$line" ]] || return 0
    value="${line#*=}"
    if [[ "$value" == \"*\" && "$value" == *\" ]]; then
        value="${value:1:${#value}-2}"
    elif [[ "$value" == \'*\' && "$value" == *\' ]]; then
        value="${value:1:${#value}-2}"
    fi
    printf '%s' "$value"
}

load_existing_config() {
    local file="${1:-/etc/ts3stream/control.env}"
    [[ -r "$file" ]] || return 0
    local key secret client pass addr public_url
    key="$(read_env_value LIVEKIT_API_KEY "$file")"
    secret="$(read_env_value LIVEKIT_API_SECRET "$file")"
    client="$(read_env_value TS3STREAM_CLIENT_KEY "$file")"
    pass="$(read_env_value TS3_QUERY_PASSWORD "$file")"
    addr="$(read_env_value TS3_QUERY_ADDR "$file")"
    public_url="$(read_env_value TS3STREAM_PUBLIC_URL "$file")"
    if [[ "$PLAYOUT_DELAY_SET" -eq 0 ]]; then
        PLAYOUT_DELAY_MS="$(read_env_value TS3STREAM_PLAYOUT_DELAY_MS "$file")"
        PLAYOUT_DELAY_MS="${PLAYOUT_DELAY_MS:-0}"
    fi

    if [[ -n "$key" && "$API_KEY" == "ts3stream" ]]; then API_KEY="$key"; fi
    [[ -z "$API_SECRET" ]] && API_SECRET="$secret"
    [[ -z "$CLIENT_KEY" ]] && CLIENT_KEY="$client"
    [[ -z "$TS3_QUERY_PASSWORD" ]] && TS3_QUERY_PASSWORD="$pass"
    if [[ -n "$addr" && "$TS3_QUERY_PORT_SET" -eq 0 ]]; then
        TS3_QUERY_PORT="${addr##*:}"
    fi
    if [[ -z "$DOMAIN" && "$public_url" == https://* ]]; then
        DOMAIN="${public_url#https://}"
        DOMAIN="${DOMAIN%/}"
    fi
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --domain) DOMAIN="${2:-}"; shift 2 ;;
            --api-key) API_KEY="${2:-}"; shift 2 ;;
            --api-secret) API_SECRET="${2:-}"; shift 2 ;;
            --client-key) CLIENT_KEY="${2:-}"; shift 2 ;;
            --ts3-query-password) TS3_QUERY_PASSWORD="${2:-}"; shift 2 ;;
            --ts3-query-port) TS3_QUERY_PORT="${2:-}"; TS3_QUERY_PORT_SET=1; shift 2 ;;
            --ts3-dir) TS3_SERVER_DIR="${2:-}"; shift 2 ;;
            --reset-ts3-query) RESET_TS3_QUERY=1; shift ;;
            --version) CONTROL_VERSION="${2:-}"; shift 2 ;;
            --playout-delay-ms) PLAYOUT_DELAY_MS="${2:-}"; PLAYOUT_DELAY_SET=1; shift 2 ;;
            --livekit-archive) LIVEKIT_ARCHIVE="${2:-}"; shift 2 ;;
            --no-caddy) INSTALL_CADDY=0; shift ;;
            --no-firewall) CONFIGURE_FIREWALL=0; shift ;;
            --skip-dns-check) SKIP_DNS_CHECK=1; shift ;;
            --ddns-update-url) DDNS_UPDATE_URL="${2:-}"; shift 2 ;;
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

check_domain_dns() {
    DNS_WARNING=""
    [[ "${INSTALL_CADDY}" -eq 1 && "${SKIP_DNS_CHECK}" -eq 0 ]] || return 0
    command -v getent >/dev/null 2>&1 || return 0
    local resolved public_ip fqdn fqdn_ips resolved_list
    resolved="$(getent ahostsv4 "$DOMAIN" 2>/dev/null | awk '{print $1}' | sort -u || true)"
    if [[ -z "$resolved" ]]; then
        DNS_WARNING="DNS: $DOMAIN does not resolve."
        warn "$DOMAIN does not resolve; Caddy cannot obtain a TLS certificate."
        return 1
    fi
    public_ip="$(curl -4 -fsS --max-time 10 https://ifconfig.me 2>/dev/null || curl -4 -fsS --max-time 10 https://api.ipify.org 2>/dev/null || true)"
    [[ -n "$public_ip" ]] || return 0
    resolved_list="$(tr '\n' ' ' <<< "$resolved")"
    if ! grep -qx "$public_ip" <<< "$resolved"; then
        DNS_WARNING="DNS: $DOMAIN -> ${resolved_list}, but this server is $public_ip."
        warn "DNS mismatch: $DOMAIN resolves to ${resolved_list}, but this server's public IP is $public_ip."
        fqdn="$(hostname -f 2>/dev/null || hostname 2>/dev/null || true)"
        if [[ -n "$fqdn" && "$fqdn" != "$DOMAIN" ]]; then
            fqdn_ips="$(getent ahostsv4 "$fqdn" 2>/dev/null | awk '{print $1}' | sort -u || true)"
            if grep -qx "$public_ip" <<< "$fqdn_ips"; then
                warn "This server's own name resolves correctly: $fqdn"
                warn "You can rerun with: --domain $fqdn"
            fi
        fi
        if command -v dig >/dev/null 2>&1; then
            local apex="${DOMAIN#*.}"
            if dig +short NS "$apex" 2>/dev/null | grep -q 'afraid.org'; then
                warn "DNS provider: FreeDNS (afraid.org). Change the A record for $DOMAIN to $public_ip at https://freedns.afraid.org/."
            fi
        fi
        warn "Caddy cannot obtain a certificate until the A record for $DOMAIN points to $public_ip."
        return 1
    fi
    return 0
}

ask_settings() {
    load_existing_config
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

    if [[ -n "${DDNS_UPDATE_URL}" ]]; then
        log "updating dynamic DNS"
        if curl -fsS --max-time 15 "${DDNS_UPDATE_URL}" >/dev/null; then
            log "dynamic DNS update request sent"
            sleep 3
        else
            warn "dynamic DNS update request failed"
        fi
    fi

    check_domain_dns || true

    if [[ -z "${API_SECRET}" ]]; then
        API_SECRET="$(openssl rand -hex 32)"
        log "generated LiveKit API secret"
    fi
    if [[ -z "${CLIENT_KEY}" ]]; then
        CLIENT_KEY="$(openssl rand -hex 64)"
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
        if is_no "${answer}"; then
            die "cancelled"
        fi
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

find_ts3_service() {
    systemctl list-unit-files --type=service --no-legend --no-pager 2>/dev/null \
        | awk '{print $1}' | grep -Ei '(^|[-_])(ts3server|teamspeak).*\.service$' | head -n 1 || true
}

ts3_query_port_from_dir() {
    local dir="$1" ini port
    for ini in "$dir/ts3server.ini" "$dir"/*.ini; do
        [[ -f "$ini" ]] || continue
        port="$(sed -n 's/^[[:space:]]*query_port[[:space:]]*=[[:space:]]*\([0-9]\+\).*/\1/p' "$ini" | head -n 1)"
        if [[ -n "$port" ]]; then
            printf '%s\n' "$port"
            return 0
        fi
    done
}

query_escape() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value// /\\s}"
    value="${value//|/\\p}"
    value="${value//\//\\/}"
    printf '%s' "$value"
}

query_login_ok() {
    local password="$1" port="${2:-10011}" escaped
    escaped="$(query_escape "$password")"
    timeout 5 bash -c '
        exec 3<>/dev/tcp/127.0.0.1/"$2" || exit 1
        IFS= read -r -t 2 _ <&3 || true
        printf "login serveradmin %s\n" "$1" >&3
        while IFS= read -r -t 2 line <&3; do
            case "$line" in
                *"error id=0"*) exit 0 ;;
                *"error id="*) exit 1 ;;
            esac
        done
        exit 1
    ' _ "$escaped" "$port" >/dev/null 2>&1
}

query_port_open() {
    timeout 1 bash -c 'exec 3<>/dev/tcp/127.0.0.1/"$1"' _ "$1" 2>/dev/null
}

accept_ts3_password() {
    local password="$1" source="$2" port="${3:-$TS3_QUERY_PORT}"
    if query_login_ok "$password" "$port"; then
        TS3_QUERY_PASSWORD="$password"
        log "detected a working TS3 ServerQuery password in $source"
        return 0
    fi
    if ! query_port_open "$port"; then
        TS3_QUERY_PASSWORD="$password"
        warn "found a TS3 ServerQuery password in $source, but the query port is not open; keeping it"
        return 0
    fi
    warn "found a TS3 ServerQuery password in $source, but it was rejected; continuing"
    return 1
}

wait_query_port() {
    local port="$1" i
    for i in {1..30}; do
        if timeout 1 bash -c 'exec 3<>/dev/tcp/127.0.0.1/"$1"' _ "$port" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

print_ts3_diagnostics() {
    local service="$1" dir="$2" port="$3"
    warn "TS3 service: ${service}; directory: ${dir}; query port: ${port}"
    systemctl status "$service" --no-pager -l 2>&1 | head -n 15 >&2 || true
    warn "TS3 service settings:"
    systemctl show -p User -p WorkingDirectory -p ExecStart --value "$service" >&2 || true
    warn "Recent ${service} log:"
    journalctl -u "$service" -n 20 --no-pager >&2 || true
}

reset_ts3_query_password() {
    local service dir user newpass output binary arg port argv exec_path exec_dir candidate
    service="$(find_ts3_service)"
    if [[ -z "$service" ]]; then
        warn "cannot reset ServerQuery password: no ts3server/teamspeak systemd service found"
        warn "set TS3_QUERY_PASSWORD manually in /etc/ts3stream/control.env"
        return 1
    fi
    dir="$TS3_SERVER_DIR"
    if [[ -z "$dir" ]]; then
        dir="$(systemctl show -p WorkingDirectory --value "$service" 2>/dev/null || true)"
        [[ "$dir" == "-" ]] && dir=""
    fi

    binary=""
    if [[ -n "$dir" && -d "$dir" ]]; then
        for candidate in "$dir/ts3server" "$dir/ts3server_linux_amd64" "$dir/ts3server_linux_x86_64" "$dir/ts3server_linux_arm64"; do
            if [[ -f "$candidate" && -x "$candidate" ]]; then
                binary="$candidate"
                break
            fi
        done
    fi
    if [[ -z "$binary" ]]; then
        # WorkingDirectory may be unset; fall back to the ExecStart path.
        exec_path="$(systemctl show -p ExecStart --value "$service" 2>/dev/null | sed -n 's/.*path=\([^ ]*\).*/\1/p' | head -n 1 || true)"
        if [[ -n "$exec_path" ]]; then
            exec_dir="$(dirname "$exec_path")"
            for candidate in "$exec_dir/ts3server" "$exec_path"; do
                if [[ -f "$candidate" && -x "$candidate" && "$(basename "$candidate")" != *.sh ]]; then
                    binary="$candidate"
                    dir="$exec_dir"
                    break
                fi
            done
        fi
    fi
    if [[ -z "$binary" ]]; then
        warn "cannot reset ServerQuery password: ts3server binary not found (use --ts3-dir)"
        warn "working directory: ${dir:-unknown}"
        return 1
    fi

    # TS3 keeps its database relative to the working directory. Start the
    # temporary server there and reuse the service's ini/database arguments,
    # otherwise the reset writes to a different, temporary database.
    local -a extra=()
    argv="$(systemctl show -p ExecStart --value "$service" 2>/dev/null | sed -n 's/.*argv\[\]=\([^;]*\).*/\1/p' | head -n 1 || true)"
    if [[ -n "$argv" ]] && printf '%s' "$argv" | grep -qE '(^|/)(docker|podman)( |$)'; then
        warn "TS3 appears to run in a container; automatic ServerQuery reset is not supported."
        warn "Set TS3_QUERY_PASSWORD manually in /etc/ts3stream/control.env."
        return 1
    fi
    for arg in $argv; do
        case "$arg" in
            inifile=*|dbsqlitedb=*|dbplugin=*|dbpluginparameter=*|dbsqlpath=*|dbsqlcreatepath=*)
                extra+=("$arg")
                ;;
        esac
    done

    port="$(ts3_query_port_from_dir "$dir" || true)"
    [[ -n "$port" ]] || port="$TS3_QUERY_PORT"
    user="$(systemctl show -p User --value "$service" 2>/dev/null || true)"
    [[ -n "$user" && "$user" != "-" ]] || user=root
    newpass="$(openssl rand -hex 24)"
    output="$(mktemp)"

    log "stopping $service to reset the ServerQuery password"
    systemctl stop "$service" >/dev/null 2>&1 || true
    local i
    for i in {1..10}; do
        systemctl is-active --quiet "$service" || break
        sleep 1
    done

    local -a cmd=("$binary")
    if ((${#extra[@]})); then cmd+=("${extra[@]}"); fi
    cmd+=("serveradmin_password=$newpass" "license_accepted=1")

    if [[ "$user" == "root" ]]; then
        ( cd "$dir" && timeout 25 "${cmd[@]}" ) >"$output" 2>&1 || true
    elif command -v runuser >/dev/null 2>&1; then
        ( cd "$dir" && timeout 25 runuser -u "$user" -- "${cmd[@]}" ) >"$output" 2>&1 || true
    else
        warn "runuser not found; cannot reset as user $user"
        rm -f "$output"
        systemctl start "$service" >/dev/null 2>&1 || true
        return 1
    fi

    if ! grep -qiE 'updated server.?query admin password|serveradmin password' "$output"; then
        warn "the temporary TS3 server did not confirm the password update"
        tail -n 15 "$output" >&2 || true
    fi

    systemctl start "$service" >/dev/null 2>&1 || true
    if ! wait_query_port "$port"; then
        warn "TS3 ServerQuery port ${port} is not listening after the restart"
        print_ts3_diagnostics "$service" "$dir" "$port"
        rm -f "$output"
        return 1
    fi
    if ! query_login_ok "$newpass" "$port"; then
        warn "TS3 ServerQuery rejected the generated password"
        print_ts3_diagnostics "$service" "$dir" "$port"
        rm -f "$output"
        return 1
    fi

    rm -f "$output"
    TS3_QUERY_PASSWORD="$newpass"
    TS3_QUERY_PORT="$port"
    TS3_SERVER_DIR="$dir"
    log "ServerQuery password reset and verified; the new password is stored in control.env"
    return 0
}

adopt_ts3_dir() {
    local dir="$1" port
    TS3_SERVER_DIR="$dir"
    if [[ "$TS3_QUERY_PORT_SET" -eq 0 ]]; then
        port="$(ts3_query_port_from_dir "$dir" || true)"
        [[ -n "$port" ]] && TS3_QUERY_PORT="$port"
    fi
}

detect_ts3_query() {
    if [[ -n "${TS3_QUERY_PASSWORD}" ]]; then
        if query_login_ok "${TS3_QUERY_PASSWORD}" "$TS3_QUERY_PORT"; then
            log "using provided TS3 ServerQuery password"
            return 0
        fi
        if ! query_port_open "$TS3_QUERY_PORT"; then
            log "using provided TS3 ServerQuery password (query port is not open yet)"
            return 0
        fi
        warn "the provided TS3 ServerQuery password was rejected; searching for a working one"
        TS3_QUERY_PASSWORD=""
    fi
    local service
    service="$(find_ts3_service)"
    local dirs=("${TS3_SERVER_DIR}" /opt/ts3server /opt/teamspeak /opt/teamspeak3-server /root/ts3server /root/teamspeak3-server /root/teamspeak /home/ts3/ts3server /home/teamspeak/teamspeak3-server /usr/local/ts3server /srv/ts3server)
    local dir line found candidate
    for dir in "${dirs[@]}"; do
        [[ -n "$dir" && -d "$dir" ]] || continue
        line="$(grep -RhoE 'password= "[^"]+"' "$dir"/logs "$dir"/*.out "$dir"/*.err 2>/dev/null | tail -n 1 || true)"
        if [[ -n "$line" ]]; then
            adopt_ts3_dir "$dir"
            candidate="${line#*password= \"}"
            candidate="${candidate%\"}"
            if accept_ts3_password "$candidate" "$dir"; then
                return 0
            fi
        fi
    done
    # Fall back to the systemd service's WorkingDirectory or a shallow search.
    if [[ -n "$service" ]]; then
        found="$(systemctl show -p WorkingDirectory --value "$service" 2>/dev/null || true)"
        [[ "$found" == "-" ]] && found=""
        if [[ -n "$found" && -d "$found" ]]; then
            dirs+=("$found")
        fi
    fi
    found="$(find /opt /srv /home /root /usr/local -maxdepth 4 -type f -name ts3server 2>/dev/null | head -n 1 || true)"
    if [[ -n "$found" ]]; then
        dirs+=("$(dirname "$found")")
    fi
    for dir in "${dirs[@]}"; do
        [[ -n "$dir" && -d "$dir" ]] || continue
        line="$(grep -RhoE 'password= "[^"]+"' "$dir"/logs "$dir"/*.out "$dir"/*.err 2>/dev/null | tail -n 1 || true)"
        if [[ -n "$line" ]]; then
            adopt_ts3_dir "$dir"
            candidate="${line#*password= \"}"
            candidate="${candidate%\"}"
            if accept_ts3_password "$candidate" "$dir"; then
                return 0
            fi
        fi
    done
    if command -v journalctl >/dev/null 2>&1; then
        line="$(journalctl -u "${service:-ts3server}" -n 2000 --no-pager 2>/dev/null | grep -oE 'password= "[^"]+"' | tail -n 1 || true)"
        if [[ -n "$line" ]]; then
            candidate="${line#*password= \"}"
            candidate="${candidate%\"}"
            if accept_ts3_password "$candidate" "${service:-ts3server} journal"; then
                return 0
            fi
        fi
    fi
    if [[ "${RESET_TS3_QUERY}" -eq 1 ]]; then
        reset_ts3_query_password && return 0
        return 1
    fi
    if [[ -t 0 ]]; then
        echo
        echo "TS3 ServerQuery password was not found in the logs."
        echo "It is printed only once when the TS3 server is first created."
        read -r -s -p "Enter the current ServerQuery password (empty = reset it): " queryAnswer
        echo
        if [[ -n "${queryAnswer}" ]]; then
            if query_login_ok "${queryAnswer}" "$TS3_QUERY_PORT"; then
                TS3_QUERY_PASSWORD="${queryAnswer}"
                return 0
            fi
            warn "ServerQuery rejected that password"
        fi
        read -r -p "Reset the ServerQuery password automatically? The TS3 server will be restarted. [Y/n] " resetAnswer
        if is_no "${resetAnswer}"; then
            warn "ServerQuery auto-provisioning disabled"
            return 1
        fi
        reset_ts3_query_password && return 0 || return 1
    fi
    warn "TS3 ServerQuery password was not found. Auto-provisioning and UID checks will be disabled; pass --ts3-query-password or --reset-ts3-query to enable them."
    return 1
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
    local public_url="https://${DOMAIN}"
    if [[ "${INSTALL_CADDY}" -eq 0 ]]; then
        control_addr="0.0.0.0:8081"
        livekit_url="ws://${DOMAIN}:7880"
        public_url="http://${DOMAIN}:8081"
    fi

    cat > /etc/ts3stream/control.env <<EOF
TS3STREAM_CONTROL_ADDR=${control_addr}
TS3STREAM_CLIENT_KEY=${CLIENT_KEY}
TS3STREAM_PLAYOUT_DELAY_MS=${PLAYOUT_DELAY_MS}
LIVEKIT_API_KEY=${API_KEY}
LIVEKIT_API_SECRET=${API_SECRET}
LIVEKIT_URL=${livekit_url}
TS3STREAM_PUBLIC_URL=${public_url}
EOF

    if [[ -n "${TS3_QUERY_PASSWORD}" ]]; then
        cat >> /etc/ts3stream/control.env <<EOF
TS3STREAM_WELCOME_AUTO=1
TS3_QUERY_ADDR=127.0.0.1:${TS3_QUERY_PORT}
TS3_QUERY_USER=serveradmin
TS3_QUERY_PASSWORD="${TS3_QUERY_PASSWORD}"
TS3_QUERY_SID=1
EOF
    else
        echo "TS3STREAM_WELCOME_AUTO=0" >> /etc/ts3stream/control.env
    fi
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
{
    acme_ca https://acme-v02.api.letsencrypt.org/directory
}

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
    local service
    service="$(find_ts3_service)"
    if [[ -n "$service" ]] && systemctl is-active --quiet "$service" 2>/dev/null; then
        log "$service is running"
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
    if [[ "${INSTALL_CADDY}" -eq 1 ]] && curl -fsS --max-time 5 http://127.0.0.1:8081/api/health >/dev/null 2>&1; then
        warn "control API is healthy locally; ${url} failed (check DNS and the TLS certificate)."
    fi
    warn "health check did not pass yet; recent control log:"
    journalctl -u ts3stream-control -n 30 --no-pager >&2 || true
    return 1
}

print_result() {
    local control_url livekit_url dns_note=""
    if [[ "${INSTALL_CADDY}" -eq 1 ]]; then
        control_url="https://${DOMAIN}"
        livekit_url="wss://${DOMAIN}"
    else
        control_url="http://${DOMAIN}:8081"
        livekit_url="ws://${DOMAIN}:7880"
    fi
    if [[ -n "${DNS_WARNING}" ]]; then
        dns_note=$'\n'"DNS/TLS warning:"$'\n'"  ${DNS_WARNING}"$'\n'"  Fix the A record for ${DOMAIN}, then rerun the installer to obtain a certificate."$'\n'
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

Auto-provisioning (Welcome + UID check):
  $([[ -n "${TS3_QUERY_PASSWORD}" ]] && echo "enabled" || echo "disabled (no ServerQuery password)")

Ports that must be reachable by clients:
  7881/tcp, 7882/udp
  $([[ ${INSTALL_CADDY} -eq 1 ]] && echo "443/tcp (Caddy / WSS / HTTPS)" || echo "8081/tcp, 7880/tcp (no TLS mode)")
${dns_note}
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
    detect_ts3_query || true
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

