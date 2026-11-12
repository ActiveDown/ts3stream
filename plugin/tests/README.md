# Viewer regression tests

Configure the plugin with `-DTS3STREAM_BUILD_UI_TESTS=ON` using the same MSVC/Qt toolchain as the plugin, then build. Put Qt's `bin` directory on `PATH`.

`fullscreen_window_test.exe` opens real native windows and exercises every connected monitor. It checks embedded/detached fullscreen placement, normal/maximized restore, host-window availability, Detach button dispatch and native close dispatch. It exits nonzero on failure; do not run on an unattended desktop.

`stream_ended_test.exe <tokens.json> <mode>` requires an isolated LiveKit test room. Modes: `0` embedded, `1` detached, `2` fullscreen. Supply JSON keys `url`, `targetIdentity`, `publisherToken`, `otherToken`, `viewerToken`; all three tokens must address the same room with distinct identities and appropriate publish/subscribe grants. Put runtime LiveKit DLLs in `%APPDATA%\TS3Client\plugins\ts3stream` (a temporary APPDATA directory is recommended). The test transmits synthetic frames, disconnects a non-target participant, then the publisher, and verifies exactly one end notification, viewer deletion and continued host visibility. Never commit token files.

These tests use the real window helpers, StreamPanel and LiveKitEngine. Final in-client validation still requires restarting TS3 with the rebuilt plugin.

Add `static` as the fourth argument to `stream_ended_test.exe` to send exactly one frame before the viewer joins, then no source updates. This reproduces the late-subscriber/static-window issue; the viewer must still receive video and the stream-ended callback.

`live_stream_menu_test.exe` replaces the entire stream registry while a LIVE popup is open and verifies that its selection still holds the original stream data.

`viewer_controls_test.exe` checks software PCM gain (including signed limits), mute/volume callbacks, fullscreen controls with the members bar hidden, toolbar stream counts/menu snapshots, and LIVE badge geometry in a narrowed/scrolled viewport.
