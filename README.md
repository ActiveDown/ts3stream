# TS3Stream

Discord-style screen and application streaming inside **stock TeamSpeak 3** on Windows, with LiveKit running next to the untouched TS3 server.

[Русская версия](README.ru.md)

## Features

- **Stock TS3 client only**: a regular TeamSpeak 3 plugin (API 26, x64) for TS3 3.6.2. Neither the client nor the server binary is patched.
- **Stream toolbar button**, enabled only while connected to a TS3 server.
- **One stream per client**; starting a second stream is blocked in the UI and in code.
- **30 / 60 / Auto FPS**, 1080p capture, adaptive H.264 bitrate up to 12 Mbit/s.
- **Application or system audio** via WASAPI process loopback; TeamSpeak audio is excluded and audio fails closed if a second TS3 client appears.
- **One LiveKit room per TS3 channel**, red LIVE badges, in-client viewer.
- **Three explicit ways to watch**: client context menu, client info button, LIVE badge popup. Row clicks only select the client.
- **Viewing and publishing are independent**: starting a stream does not open the viewer; closing the viewer does not stop the stream.
- **Fullscreen** on the TS3 window monitor, with a semi-transparent members bar and large controls.
- **Crash-free TS3 shutdown**, including while publishing and watching.

## Quick install

### Linux server (automatic installer)

Requirements:

- Ubuntu 22.04/24.04 or Debian 12, x86_64;
- root/sudo;
- a DNS name, for example `stream.example.com`, pointing to the server;
- ports 80/443 open for Let's Encrypt;
- the **stock TS3 server already installed and running** — the installer does not modify it.

Download the installer, inspect it, and run it:

```bash
curl -fsSL https://raw.githubusercontent.com/ActiveDown/ts3stream/main/deploy/install-ts3stream.sh -o install-ts3stream.sh
less install-ts3stream.sh
sudo bash install-ts3stream.sh
```

The installer prompts for the domain and confirmation, then:

- installs LiveKit and the `ts3stream-control` API;
- installs Caddy and obtains a TLS certificate;
- creates `livekit` and `ts3stream` systemd services;
- generates the LiveKit API secret and the client shared key;
- adds UFW rules for the streaming ports;
- prints a ready `service.json` for the Windows client.

Non-interactive examples:

```bash
# Internet-facing with automatic TLS
sudo bash install-ts3stream.sh --domain stream.example.com --yes

# LAN / test without TLS (IP is allowed here)
sudo bash install-ts3stream.sh --domain 192.168.1.50 --no-caddy --yes
```

Useful options: `--version v0.2.1` to pin the control API release, `--no-firewall`, `--help`.

After installation:

```bash
systemctl status livekit ts3stream-control caddy
curl https://stream.example.com/api/health
```

Detailed manual steps and troubleshooting: [`docs/install-linux.en.md`](docs/install-linux.en.md).  
Windows server: [`docs/install-windows.en.md`](docs/install-windows.en.md).

### Windows server

There is no one-command installer for Windows yet; the step-by-step guide covers:

- stock TS3 server on Windows;
- LiveKit for Windows;
- `ts3stream-control.exe`;
- Windows Firewall rules;
- optional Caddy TLS and NSSM services.

See [`docs/install-windows.en.md`](docs/install-windows.en.md).

### Windows client

1. Close TeamSpeak 3.
2. Install `ts3stream-0.2.1.ts3_plugin` with the TeamSpeak Package Installer.
3. Start TS3, connect to the server, press **Stream**.
4. Create `%APPDATA%\TS3Client\plugins\ts3stream\service.json`:

```json
{
  "controlUrl": "https://stream.example.com",
  "clientKey": "PASTE_CLIENT_KEY"
}
```

The installer prints the exact JSON with the generated key.

## How it works

```
TS3 client + plugin ──HTTPS/WSS──▶ Caddy ──▶ LiveKit (SFU) + control API
        │                              │
        └── WebRTC media (UDP/TCP) ────▶ LiveKit
```

- The plugin asks the control API for a JWT, joins the LiveKit room of its TS3 channel, and publishes the screen + audio tracks.
- The control API holds the LiveKit secret, issues tokens, and keeps a 15-second TTL registry of active streams for LIVE badges.
- Viewers get a subscribe-only token and filter the selected participant by identity.
- TS3 server ports (9987/UDP, 10011/TCP, 30033/TCP) are untouched.

## Build from source (developers)

Requirements: Windows 10 2004+, Visual Studio 2022 Build Tools (MSVC 14.4x), Windows SDK, Qt 5.15.2 `msvc2019_64`, CMake, Ninja, Go 1.22+.

The build scripts expect a portable toolchain:

```
%USERPROFILE%\ts3stream-toolchain\
  msvc\
  winsdk\
  Qt\5.15.2\msvc2019_64\
  cmake\...\bin\cmake.exe
  ninja\ninja.exe
```

Set `TS3STREAM_TOOLCHAIN` to override the path.

```powershell
powershell -File tools\setup-livekit.ps1
powershell -File plugin\build.ps1
powershell -File tools\package.ps1
```

The result is `dist\ts3stream-<version>.ts3_plugin`.

## Repository layout

```
plugin/   C++ TS3 plugin (Qt 5.15.2, LiveKit C++ SDK, WGC, WASAPI)
service/  Go control API (tokens, live registry)
deploy/   Linux installer, systemd, LiveKit YAML, Caddy
tools/    build/package/setup scripts and test tools
docs/     Linux/Windows installation, architecture, audio
```

## Limitations

- Windows x64 and TS3 3.6.2 only.
- The Linux installer supports x86_64 Debian/Ubuntu; other distributions need the manual guide.
- TURN/TLS is optional and requires a separate certificate; see the Linux guide.
- System audio requires exactly one running TS3 client; otherwise audio fails closed.
- Hardware H.264 encoding depends on the GPU driver; the SDK can fall back to software.

## License

MIT for the original code. TeamSpeak 3 SDK headers remain property of TeamSpeak Systems GmbH. LiveKit is Apache-2.0; Qt is LGPL-3.0. See [NOTICE](NOTICE).
