# TS3Stream LiveKit server fork

Base: LiveKit **v1.13.11**, commit `385aa40f9292fe1db8caad87301e4b285b7c198a` (Apache-2.0).
Fork version: **1.13.11-ts3stream.4** (bundled with TS3Stream 0.3.10). `server-av-buffer.patch` contains all changes, including tests; it applies to the exact pinned upstream tree. No untracked local SDK replacement is required.

Build with Go 1.26+ and Git:

```powershell
.\server\livekit\build.ps1 -WorkDir C:\build\ts3stream-livekit-new
```

The script checks the base commit, validates/applies the patch, runs server-buffer regressions, and builds Windows/Linux amd64 binaries into `dist`. It requires a fresh work directory and never resets an existing checkout.

On Linux, clone the pinned tag, verify `git rev-parse HEAD`, `git apply --check /path/server-av-buffer.patch`, then `git apply` it. Run `go test ./pkg/sfu ./pkg/sfu/buffer -run '^TestServerAV'`, and `CGO_ENABLED=0 go build -trimpath -ldflags '-s -w' -o ts3stream-livekit-linux-amd64 ./cmd/server`.

## Changes

- The signed **new-room** metadata contract `ts3stream-server-av-v1:2000` enables actual server-side RTP retention. Ordinary rooms and legacy `ts3stream-av-delay-v1` rooms retain their original behavior.
- The publisher's audio and video share a server clock. RTCP sender reports map their different RTP clock rates to a common sender NTP timeline; absolute deadlines provide the configured delay and absorb uplink timing variation. Until sender reports arrive, both tracks use arrival time plus the configured delay.
- The SFU retains RTP in its existing bounded ingress packet cache (16,384 video / 2,048 audio packets per stream/layer). Server-mode tracks allocate this capacity when binding, rather than relying on normal one-second adaptive growth; increasing the maximum alone does not retain the delayed packets. Metadata queues are also bounded. Original RTP/RTCP timestamp relationships are preserved. Outbound retransmissions continue to use the retained packet cache.
- Waits are interruptible on close/restart; packets over 250 ms past their deadline are discarded instead of replaying stale backlog. Video requests a throttled keyframe after such a drop. Ending a publication discards its buffered tail.
- Server-mode metadata queues order packets by extended RTP sequence number. The forwarder rechecks the queue after waiting, including repairs received during that wait, so uplink reordering is repaired within the holdback window rather than replayed to every viewer. Ordinary rooms remain FIFO. Regression coverage includes rollover, held-packet ownership, and a normalized real header-order trace with a 227 ms late frame tail.
- `/ts3stream/buffering` advertises the protocol. Control API checks it before issuing a server-mode token and refuses a stock/incompatible backend. A server-mode room with additional client playout delay is rejected.
- Client PCM delay and multi-second client video playout bounds are **not** requested in this mode. Small WebRTC/device buffers remain necessary.
- Revision 4 packages the previously deployed revision 3 diagnostics: per-track read/SR/reselection/late-drop/zero-write counters and maximum hold/lateness, logged every five seconds. Forwarding behavior is unchanged from `1.13.11-ts3stream.3-diagnostic`. It does not claim to fix all remaining ingress losses or WAN freezes.

## Limits

This is a single-node TS3Stream screen-sharing path. It does not synchronize different publishers into one mixed program. Server buffering cannot absorb loss/congestion **after** the server, fix a stalled source, or control physical display/speaker latency. No universal smoothness or lip-sync guarantee is implied. Server-side waiting cannot prevent a slow viewer's own network/decoder from falling behind. Avoid changing reserved room metadata on a live room; create a fresh room when changing modes.

See [server-buffered-playback.md](../../docs/server-buffered-playback.md) for deployment and measured results.
