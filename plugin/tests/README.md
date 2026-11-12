# Viewer regression tests

`source_picker_test.exe` uses deterministic monitor/window fixtures to exercise all three resolution and FPS selections in the real picker, its 1080p default and cancellation. It checks the production scaling helper on 4K, portrait, ultrawide, small and odd-sized frames (aspect ratio, resolution limits and no upscaling). No capture or server connection is started.

Configure the plugin with `-DTS3STREAM_BUILD_UI_TESTS=ON` using the same MSVC/Qt toolchain as the plugin, then build. Put Qt's `bin` directory on `PATH`.

`fullscreen_window_test.exe` opens real native windows and exercises every connected monitor. It checks embedded/detached fullscreen placement, normal/maximized restore, host-window availability, Detach button dispatch and native close dispatch. It exits nonzero on failure; do not run on an unattended desktop.

`stream_ended_test.exe <tokens.json> <mode>` requires an isolated LiveKit test room. Modes: `0` embedded, `1` detached, `2` fullscreen. Supply JSON keys `url`, `targetIdentity`, `publisherToken`, `otherToken`, `viewerToken`; all three tokens must address the same room with distinct identities and appropriate publish/subscribe grants. Put runtime LiveKit DLLs in `%APPDATA%\TS3Client\plugins\ts3stream` (a temporary APPDATA directory is recommended). The test transmits synthetic frames, disconnects a non-target participant, then the publisher, and verifies exactly one end notification, viewer deletion and continued host visibility. Never commit token files.

These tests use the real window helpers, StreamPanel and LiveKitEngine. Final in-client validation still requires restarting TS3 with the rebuilt plugin.

Add `static` as the fourth argument to `stream_ended_test.exe` to send exactly one frame before the viewer joins, then no source updates. This reproduces the late-subscriber/static-window issue; the viewer must still receive video and the stream-ended callback.

Use `resize` instead of `static` to send eight size/content stages over the same published track, including 1920×1044 → 1920×1080 → 2560×1440 → 1920×1044. The receiver must observe both the new dimensions and the new pixels at every stage, without a premature stream-ended event. Before the fix this times out at stage 1 while receiving repeated old frames.

`wgc_resize_test.exe` captures its own animated native window, enters/exits fullscreen and resizes it (including odd dimensions). It verifies fresh center/corner pixels across seven stages. Run on an interactive desktop, with temporary `APPDATA` for its diagnostic log. The test keeps the window on-screen because WGC may black out off-monitor areas.

`viewer_autohide_test.exe` exercises the actual idle timer, movement/keyboard wake, hover and slider-drag protection, manual member visibility and leaving fullscreen from embedded/detached hosts. It defaults to Qt's offscreen platform so real desktop mouse input cannot interfere with hover assertions. The production delay is checked as 3000 ms, then shortened for the test.

`live_stream_menu_test.exe` replaces the entire stream registry while a LIVE popup is open and verifies that its selection still holds the original stream data.

`viewer_controls_test.exe` checks software PCM gain through 125% (including saturation at both signed limits without overflow), 0–125% slider/label/callbacks, the unchanged 100% default, boost preservation across mute/fullscreen, fullscreen controls with the members bar hidden, toolbar stream counts/menu snapshots, and LIVE badge geometry in a narrowed/scrolled viewport.

`desktop_frame_test.exe` is deterministic and does not capture the desktop. It checks all four DXGI rotations, row padding, color/alpha/masked/monochrome pointers, hotspot semantics, clipping and incomplete pointer data.

`video_send_queue_test.exe` feeds bursts into the production encoder-input queue and checks newest-frame replacement during the pacing wait, release of stale allocations, the frame-rate deadline, idle keepalive, interruptible stop and clean restart. This is sender pacing, not a two-second receive/jitter buffer; it cannot recover frames already dropped by WebRTC.

`rtc_diagnostics_test.exe` verifies interval bitrate/FPS, encode/decode and packet/jitter-buffer delay, loss/freeze deltas, single-PC duplicate suppression, counter reset, idle intervals and disappearing/reappearing tracks. No server is required.

`delayed_audio_test.exe` checks the room metadata contract, no early output, copied PCM/order, bounded memory under overload, pause/reset/restart and interruptible shutdown without playing the queued tail. No audio device or server is used.

`scene_change_test.exe <tokens.json>` uses the same isolated-room token schema as `stream_ended_test`, plus optional `expectedDelayMs`, `viewerStartMs`, `reconnectAtMs`. It publishes six synthetic 1080p scenes with motion and visual frame IDs, plus five frequency-identified audio pulses; no screen or microphone is captured. A second publisher is already active. By default the selected publisher starts after the viewer joins; optional late-join/reconnect scenarios intentionally omit some content during the gap. It verifies scene/frame/pulse delivery, selected subscription and actual room mode. With `expectedDelayMs > 0` it also asserts both median callback delays are within -150/+500 ms of the configured value and their difference is below 150 ms. These are callback measurements, not physical screen/speaker latency or congested-WAN guarantees. Output distinguishes all-session gaps (including startup/reconnection fill) from post-warmup gaps.

Use the new control API with `TS3STREAM_PLAYOUT_DELAY_MS=2000` to issue tokens for a fresh local room, or signed JWT `roomConfig` fields `minPlayoutDelay=2000`, `maxPlayoutDelay=2000`, `syncStreams=true`, `metadata="ts3stream-av-delay-v1:2000"`. Do not substitute a global server delay without matching metadata. Also test an existing room created by an unbuffered publisher token, then join with buffered viewer tokens and `expectedDelayMs=0`; this checks actual room settings take precedence. Keep generated config/tokens outside the repository and delete them after running.

`desktop_capture_test.exe` exercises the actual DXGI backend on every connected monitor, independently of OS version. It briefly shows a small topmost test window, verifies fresh pixels, monitor dimensions, timestamps, 30/60 FPS configuration, stop/restart and invalid-monitor errors. Run on an interactive desktop with temporary `APPDATA`. It does not connect to a server or transmit captured content. On Windows 10 also verify in TS3 that selecting a monitor logs `wgc border control unavailable; using DXGI for monitor capture` and shows no yellow border; individual windows intentionally remain on WGC.
