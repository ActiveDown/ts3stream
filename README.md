# TS3Stream

Discord-style screen and application streaming inside **stock TeamSpeak 3** on Windows, with a LiveKit service running next to the untouched TS3 server.

[Русская версия](README.ru.md)

## Features

- **Stock TS3 client only**: a regular TeamSpeak 3 plugin (API 26, x64) for TS3 3.6.2. Neither the client nor the server binary is patched.
- **Stream toolbar button** that is disabled until the client is connected to a TS3 server.
- **One stream per client**: the UI and the code both reject a second concurrent stream.
- **30 / 60 / Auto FPS** selector in the capture dialog. Auto uses 60 FPS as a ceiling and lets WebRTC adapt.
- **1080p screen/window capture** via Windows Graphics Capture.
- **Adaptive bitrate**: up to 12 Mbit/s H.264; WebRTC congestion control lowers and raises the actual rate automatically.
- **Audio**: WASAPI process loopback for application audio and for system audio with the TeamSpeak process tree excluded.
- **Fail-closed audio safety**: the helper checks the number of `ts3client_win64.exe` processes continuously. If a second TS3 starts, system audio is disabled immediately. Raw device loopback is never used.
- **LiveKit SFU** with the official C++ SDK, H.264 video and Opus audio.
- **One LiveKit room per TS3 channel**: `ts3-<serverHash>-<channelId>`.
- **Red LIVE badge** next to the streaming nickname in the TS3 server tree.
- **Three ways to watch**, and only these:
  1. client context menu → `TS3Stream` → **Watch stream**;
  2. red **Watch stream** button in the client info panel;
  3. click the **LIVE** badge → confirmation popup.
- Starting a stream does **not** open the viewer. To watch your own stream, use one of the three explicit actions; viewing does not stop publishing.
- A plain click on a client row only selects the client; it never starts playback.
- In-client viewer panel on the right, fullscreen on the same monitor as the TS3 window (F11 / button, Esc to exit).
- Semi-transparent fullscreen members bar: channel members and talking indicators, with large **Hide** / **Exit** buttons.
- Crash-free TS3 shutdown, including while publishing or watching.

## How it works

```
TS3 client + plugin ──HTTPS/WSS──▶ Caddy ──▶ LiveKit (SFU) + control API
        │                              │
        └── WebRTC media (UDP/TCP) ────▶ LiveKit
```

- The plugin requests a JWT from the control API, joins the LiveKit room of its current TS3 channel, and publishes the screen track.
- The control API keeps a 15-second TTL registry of active streams. The plugin sends a heartbeat every 5 seconds; other clients use the registry to draw LIVE badges.
- Viewers request a subscribe-only token and filter the selected participant by identity.
- The TS3 server (9987/UDP, 10011/TCP, 30033/TCP) is not modified in any way.

## Build (Windows)

Requirements:

- Windows 10 2004 or newer, x64.
- Visual Studio 2022 Build Tools (MSVC 14.4x), Windows SDK 10.0.26100+.
- Qt 5.15.2 `msvc2019_64`.
- CMake 3.20+, Ninja.
- Go 1.22+ for the control service.
- The LiveKit C++ SDK is downloaded by `tools/setup-livekit.ps1`.

The build scripts expect a portable toolchain directory:

```
%USERPROFILE%\ts3stream-toolchain\
  msvc\
  winsdk\
  Qt\5.15.2\msvc2019_64\
  cmake\...\bin\cmake.exe
  ninja\ninja.exe
```

Set `TS3STREAM_TOOLCHAIN` to use a different location.

```powershell
# 1. Download the LiveKit C++ SDK into plugin/third_party/livekit
powershell -File tools\setup-livekit.ps1

# 2. Build the plugin and the audio helper
powershell -File plugin\build.ps1

# 3. Build the installable package
powershell -File tools\package.ps1
```

`plugin\build.ps1` also copies the DLL and helper into
`%APPDATA%\TS3Client\plugins` for local debugging.
`tools\package.ps1` produces `dist\ts3stream-<version>.ts3_plugin`.

## Install the plugin

1. Close TeamSpeak 3.
2. Open `dist\ts3stream-<version>.ts3_plugin` with the TeamSpeak Package Installer.
3. Start TS3 and connect to a server. The **Stream** button becomes active.
4. If the control API is not on `http://127.0.0.1:8081`, create
   `%APPDATA%\TS3Client\plugins\ts3stream\service.json`:

```json
{
  "controlUrl": "https://stream.example.com",
  "clientKey": "shared-secret"
}
```

The package installs the helper and LiveKit DLLs under `<TS3>\plugins\ts3stream`.
On first start the plugin copies the LiveKit DLLs to the TS3 data directory.

## Server (Linux)

The stock TS3 server stays as is. Run LiveKit, the Go control API and a TLS
reverse proxy next to it. Templates are in [`deploy/`](deploy/):

- `livekit.yaml.example` — LiveKit with ICE and TURN/TLS.
- `control.env.example` — API keys, public WSS URL, client shared key.
- `Caddyfile.example` — HTTPS control API and WSS signaling.
- `livekit.service`, `ts3stream-control.service`, `install.sh`.

Build the Linux control binary:

```powershell
cd service
$env:GOOS='linux'; $env:GOARCH='amd64'; $env:CGO_ENABLED='0'
go build -trimpath -ldflags '-s -w' -o ..\dist\linux\ts3stream-control .\cmd\control
```

See [`docs/deployment.md`](docs/deployment.md) for ports, config and systemd.

Detailed step-by-step guides:

- Linux: [`docs/install-linux.en.md`](docs/install-linux.en.md) · [`docs/install-linux.ru.md`](docs/install-linux.ru.md)
- Windows server: [`docs/install-windows.en.md`](docs/install-windows.en.md) · [`docs/install-windows.ru.md`](docs/install-windows.ru.md)

## Repository layout

```
plugin/   C++ TS3 plugin (Qt 5.15.2, LiveKit C++ SDK, WGC, WASAPI)
service/  Go control API (tokens, live registry)
deploy/   systemd, LiveKit YAML, Caddy, install.sh
tools/    build/package/setup scripts and small test tools
docs/     architecture, audio, deployment
```

## Limitations

- Windows x64 and TS3 3.6.2 only.
- 1080p is the capture ceiling; 4K is not enabled.
- System audio requires exactly one running TS3 client; otherwise audio fails closed.
- Linux deployment needs a domain and TLS certificates for WSS/TURN.
- Hardware H.264 encoding depends on the GPU driver; the SDK can fall back to software.

## License

MIT for the original code. TeamSpeak 3 SDK headers remain property of
TeamSpeak Systems GmbH and are used under their SDK terms. LiveKit is
Apache-2.0; Qt is LGPL-3.0. See [NOTICE](NOTICE).
