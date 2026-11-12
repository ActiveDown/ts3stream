#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
unset TS3STREAM_PLAYOUT_DELAY_MS
# Sourcing is deliberately side-effect-free; the installer main is guarded.
source "$root/install-linux.sh"
[[ "$PLAYOUT_DELAY_MS" == 2000 ]]
printf 'LIVEKIT_API_KEY=test\n' > "$tmp/control.env"
load_existing_config "$tmp/control.env"
[[ "$PLAYOUT_DELAY_MS" == 0 ]]
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
[[ "$PLAYOUT_DELAY_MS" == 2000 ]]
parse_args --playout-delay-ms 0
[[ "$PLAYOUT_DELAY_MS" == 0 ]]
grep -q '^TS3STREAM_PLAYOUT_DELAY_MS=2000$' "$root/control.env.example"
printf 'Linux installer fresh/default, preserved and explicit A/V delay: PASS\n'
