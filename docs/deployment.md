# Развёртывание на Linux

## Принцип

TS3-сервер не модифицируется. Рядом на той же машине работает отдельный стек:

```
/opt/ts3stream/
  bin/
    livekit-server
    ts3stream-control
  etc/
    livekit.yaml
    control.env
  systemd/
    livekit.service
    ts3stream-control.service
```

## Топология

```
TS3 Client + plugin
   │  WSS 443 (signal) + HTTPS 443 (control)
   ├──────────────────────────────▶ Caddy (TLS)
   │                                  ├── /api/*  → 127.0.0.1:8081 control
   │                                  └── /rtc*, /twirp/* → 127.0.0.1:7880 LiveKit
   │
   │  UDP 7882 (ICE mux)
   ├──────────────────────────────────▶ livekit-server
   │  TCP 7881 (ICE/TCP)
   ├──────────────────────────────────▶ livekit-server
   │  TURN 3478/UDP, 5349/TCP
   └──────────────────────────────────▶ livekit-server

TS3 Server: 9987/UDP, 10011/TCP, 30033/TCP — без изменений.
```

Комнаты создаются по одной на канал TS3: `ts3-<sha1(serverUid)[0:12]>-<channelId>`.
Control API хранит реестр активных публикаторов с TTL 15 секунд; плагин шлёт heartbeat
каждые 5 секунд. Это даёт LIVE-бейджи без запросов к ServerQuery и без модификации TS3.

## Порты

| Процесс | Порты | Назначение |
|---|---|---|
| Caddy | 80/TCP, 443/TCP | TLS/WSS, HTTPS для control API |
| livekit-server | 127.0.0.1:7880/TCP | signaling/API за Caddy |
| | 7881/TCP | ICE/TCP fallback |
| | 7882/UDP | ICE/UDP mux |
| | 3478/UDP | встроенный TURN/UDP + STUN |
| | 5349/TCP | встроенный TURN/TLS |
| ts3stream-control | 127.0.0.1:8081/TCP | токены, реестр стримов |
| ts3server | 9987/UDP, 10011/TCP, 30033/TCP | без изменений |

## Установка

1. Соберите Linux-бинарник control (на Windows):

```powershell
cd service
$env:GOOS='linux'; $env:GOARCH='amd64'; $env:CGO_ENABLED='0'
go build -trimpath -ldflags '-s -w' -o ..\dist\linux\ts3stream-control .\cmd\control
```

2. Скачайте `livekit-server` для linux/amd64 с релизов LiveKit и положите рядом
   с `ts3stream-control` в `dist/linux/`.

3. Скопируйте `deploy/*` и оба бинарника на сервер, затем:

```bash
sudo ./install.sh
```

4. Отредактируйте `/etc/ts3stream/livekit.yaml` и `/etc/ts3stream/control.env`:
   - `keys` / `LIVEKIT_API_KEY` + `LIVEKIT_API_SECRET` — одинаковые;
   - `LIVEKIT_URL=wss://stream.example.com`;
   - `TS3STREAM_CLIENT_KEY` — случайный общий ключ;
   - TURN domain и пути к сертификатам Let's Encrypt.

5. Установите Caddy и `Caddyfile.example` как конфиг сайта. Caddy сам получит
   сертификат и проксирует WSS/HTTPS.

6. Перезапустите сервисы:

```bash
sudo systemctl restart livekit ts3stream-control
sudo systemctl status livekit ts3stream-control
```

## Конфиг клиента

На каждой Windows-машине создайте
`%APPDATA%\TS3Client\plugins\ts3stream\service.json`:

```json
{
  "controlUrl": "https://stream.example.com",
  "clientKey": "тот-же-ключ-что-в-control.env"
}
```

Плагин устанавливается через `ts3stream-<version>.ts3_plugin` (TeamSpeak Package
Installer). Пакет кладёт helper и LiveKit DLL в `<TS3>\plugins\ts3stream\`;
DLL LiveKit при первом запуске копируются в `%APPDATA%\TS3Client\plugins\ts3stream`.

## API

- `GET|POST /api/token?room=&identity=&name=&publish=1|0` — JWT LiveKit.
- `POST /api/streams/start` — `{room,identity,nickname,channelId,serverUid}`.
- `POST /api/streams/heartbeat` — `{room,identity}`; 404, если запись истекла.
- `POST /api/streams/stop` — `{room,identity}`.
- `GET /api/streams?room=` — активные записи (TTL 15 с).
- `GET /api/live` — все активные записи.
- `GET /api/health`.

Если `TS3STREAM_CLIENT_KEY` задан, все `/api/*`, кроме `/api/health`, требуют
заголовок `X-TS3Stream-Key`.

## Ресурсы

Для 1 издателя 1080p30 + 10 зрителей:

- 2 vCPU, 2 ГБ RAM, 100 Мбит/с;
- CPU сервера ~0.3–0.8 ядра (без транскодинга);
- RAM ~0.3–0.8 ГБ;
- сеть: вход ~4–12 Мбит/с, выход ~40–120 Мбит/с в зависимости от битрейта.

## Проверки

- [ ] `https://stream.example.com/api/health` отвечает `{"status":"ok"}`.
- [ ] WSS LiveKit доступен через Caddy.
- [ ] Медиа идёт по UDP 7882.
- [ ] TURN работает при закрытом UDP.
- [ ] Плагин на клиенте получает токен и публикует 1080p.
- [ ] LIVE-бейдж виден другим клиентам в том же канале.
- [ ] Просмотр открывается из контекстного меню, кнопки в панели клиента и по LIVE-бейджу.
- [ ] В fullscreen панель участников показывает говорящих; «Скрыть»/«Выйти» работают.
- [ ] Выход из TS3 не создаёт crash dump.
- [ ] Сервисы переживают перезагрузку; логи настроены на ротацию.

