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

`viewer_controls_test.exe` checks software PCM gain (including signed limits), mute/volume callbacks, fullscreen controls with the members bar hidden, toolbar stream counts/menu snapshots, and LIVE badge geometry in a narrowed/scrolled viewport.
