#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
unset TS3STREAM_PLAYOUT_DELAY_MS
unset TS3STREAM_BUFFER_MODE
# Sourcing is deliberately side-effect-free; the installer main is guarded.
source "$root/install-linux.sh"
[[ "$PLAYOUT_DELAY_MS" == 2000 ]]
[[ "$BUFFER_MODE" == server ]]
printf 'LIVEKIT_API_KEY=test\n' > "$tmp/control.env"
load_existing_config "$tmp/control.env"
[[ "$PLAYOUT_DELAY_MS" == 0 ]]
[[ "$BUFFER_MODE" == client ]]
printf 'TS3STREAM_BUFFER_MODE=server\n' > "$tmp/control.env"
load_existing_config "$tmp/control.env"
[[ "$BUFFER_MODE" == server ]]
parse_args --buffer-mode client
load_existing_config "$tmp/control.env"
[[ "$BUFFER_MODE" == client ]]
printf 'TS3STREAM_PLAYOUT_DELAY_MS=3000\n' > "$tmp/control.env"
load_existing_config "$tmp/control.env"
[[ "$PLAYOUT_DELAY_MS" == 3000 ]]
parse_args --playout-delay-ms 2000
load_existing_config "$tmp/control.env"
[[ "$PLAYOUT_DELAY_MS" == 2000 ]]
parse_args --playout-delay-ms 0
load_existing_config "$tmp/control.env"
[[ "$PLAYOUT_DELAY_MS" == 0 ]]
source "$root/install-ts3stream.sh"
[[ "$BUFFER_MODE" == server ]]
parse_args --buffer-mode client
[[ "$BUFFER_MODE" == client ]]
[[ "$PLAYOUT_DELAY_MS" == 2000 ]]
parse_args --playout-delay-ms 0
[[ "$PLAYOUT_DELAY_MS" == 0 ]]
grep -q '^TS3STREAM_PLAYOUT_DELAY_MS=2000$' "$root/control.env.example"
printf 'Linux installer fresh/default, preserved and explicit A/V delay: PASS\n'
