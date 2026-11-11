# Выбор SFU

Решение: **LiveKit** — основной вариант. **mediasoup** — запасной, если LiveKit C++ SDK окажется непригодным.

## Почему LiveKit

1. **Официальный C++ SDK под Windows x64** с prebuilt-архивом.
   - `VideoSource` — сырые кадры (BGRA/I420/NV12), SDK кодирует и делает simulcast.
   - `EncodedVideoSource` — готовый H.264 Annex B, гарантированный аппаратный энкодер (Media Foundation/NVENC), но без simulcast.
   - `AudioSource` — PCM 48 kHz, SDK кодирует Opus.
   - `VideoStream` / callbacks — декодированные кадры для рендера.
2. **Сервер — один Go-бинарник**: signaling, SFU, встроенный TURN, JWT, RoomService API, webhooks.
3. **Сетевая часть**: simulcast/SVC, NACK/RTX, BWE, jitter buffer, Opus, TURN/TLS.
4. **Развёртывание**: systemd, YAML, Caddy для WSS, без Docker и без coturn.
5. **Лицензия Apache 2.0**, активная разработка.

## Компромисс: simulcast vs аппаратный H.264

- **Raw `VideoSource` + simulcast** — адаптивность под каждого зрителя, но на Windows аппаратный энкодер в LiveKit не гарантирован; 1080p30 + 3 слоя может быть тяжёлым для CPU.
- **`EncodedVideoSource` + Media Foundation/NVENC** — лёгкий CPU, но один пространственный слой; адаптивность только через несколько треков/ручное переключение.

План PoC: сделать оба режима и выбрать по результатам замеров. Для 1–10 зрителей, скорее всего, хватит pre-encoded H.264 + один слой, а при плохой сети — второй трек 540p.

## Альтернативы

| Вариант | Плюсы | Минусы | Вердикт |
|---|---|---|---|
| **LiveKit** | C++ SDK, 1 бинарник, TURN, JWT, simulcast | C++ SDK молодой; Windows hardware encode не гарантирован | **Основной** |
| mediasoup | libmediasoupclient, зрелый SFU, simulcast/SVC | Нет сервера/сигналинга/TURN; сборка libwebrtc m140 | Запасной |
| Janus | Зрелый, VideoRoom | GPLv3, нет C++ SDK, сложная обвязка | Нет |
| Pion/ion-sfu | Go, простой | Проект остановлен, нет C++ SDK | Нет |
| SRS | 1 бинарник, WHIP/WHEP | Потоковый, без per-viewer layers, нет C++ SDK | Нет |
| OvenMediaEngine | WHIP/WHEP, TURN | AGPLv3, нет C++ SDK, транскодинг для ABR | Нет |
| Ant Media | SFU/ABR | Java, WebRTC playback в Enterprise | Нет |
| Jitsi JVB | Зрелый | XMPP/Prosody/Jicofo, нет C++ SDK | Нет |
| Galene | Лёгкий, TURN | Свой WebSocket-протокол, нет C++ SDK | Нет |
| MediaMTX | 1 бинарник, WHIP/WHEP | Media-router без layers | Аварийный |

## Источники

- LiveKit C++ quickstart: https://docs.livekit.io/transport/sdk-platforms/cpp/
- LiveKit C++ SDK: https://github.com/livekit/client-sdk-cpp
- Pre-encoded video: https://docs.livekit.io/transport/media/pre-encoded/
- Hardware encoders: https://docs.livekit.io/robotics/media/video/encoders/
- Self-hosting/ports/TURN: https://docs.livekit.io/home/self-hosting/server-setup/
- mediasoup: https://mediasoup.org/documentation/v3/
- libmediasoupclient: https://mediasoup.org/documentation/v3/libmediasoupclient/installation/
- Janus: https://github.com/meetecho/janus-gateway
- ion-sfu (остановлен): https://github.com/ionorg/ion-sfu/issues/688
