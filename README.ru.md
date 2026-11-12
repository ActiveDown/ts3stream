# TS3Stream

Стриминг экрана и приложений внутри TeamSpeak 3 в стиле Discord.

## Что уже работает

- Клиент Windows: обычный плагин TS3 API 26 для стокового клиента 3.6.2 x64. Бинарники TS3 не патчатся.
- Кнопка «Стрим» в тулбаре; она недоступна, пока клиент не подключён к серверу.
- Запуск только одного стрима на клиента: повторный запуск блокируется и в UI, и в коде.
- Выбор частоты кадров при старте: 30 FPS, 60 FPS или «Авто» (до 60 FPS, потолок регулируется WebRTC).
- Захват WGC: монитор или окно приложения. Рабочее разрешение — 1080p.
- Адаптивный битрейт: верхняя планка 12 Мбит/с; WebRTC сам снижает/поднимает реальный битрейт по обратной связи REMB/TWCC.
- Звук: WASAPI process loopback для приложения и системного звука. TeamSpeak исключён из системного захвата на уровне процесса.
- Защита от петли: helper проверяет число `ts3client_win64.exe` не только на старте, но и во время работы. Если запущен второй TS3, системный звук немедленно отключается.
- LiveKit SFU + официальный C++ SDK; H.264, Opus.
- Комната LiveKit на каждый канал TS3 (`ts3-<serverHash>-<channelId>`).
- Красный LIVE-бейдж рядом с ником стримящего в дереве TS3.
- Три способа открыть просмотр:
  1. контекстное меню клиента → `TS3Stream` → «Смотреть стрим»;
  2. красная кнопка «Смотреть стрим» в панели информации о клиенте;
  3. клик по LIVE-бейджу → всплывающее подтверждение.
- Запуск стрима не открывает панель просмотра: свой стрим открывается только явно, одним из трёх способов, и просмотр не останавливает публикацию.
- Обычный клик по строке клиента стрим не открывает.
- Панель просмотра справа, полноэкранный режим (F11 / кнопка), Esc — выход.
- Fullscreen открывается на том мониторе, где находится окно TS3.
- Полупрозрачная панель участников внизу fullscreen: кто в канале и кто говорит; кнопки «Скрыть» / «Выйти» крупные и контрастные.
- Выход из TS3 без краш-репорта: проверено с активным стримом и viewer.

## Быстрый локальный запуск

```powershell
# 1. LiveKit C++ SDK (один раз)
powershell -File tools\setup-livekit.ps1

# 2. Сборка плагина и helper
powershell -File plugin\build.ps1

# 3. LiveKit Server и Control API
#    livekit-server.exe --dev
#    ts3stream-control.exe --addr 127.0.0.1:8081

# 4. Пакет для установки
powershell -File tools\package.ps1
```

`build.ps1` копирует DLL и helper в `%APPDATA%\TS3Client\plugins` для локальной отладки.
`package.ps1` собирает `dist\ts3stream-<version>.ts3_plugin` — установка через
штатный TeamSpeak Package Installer.

### Конфиг клиента

`%APPDATA%\TS3Client\plugins\ts3stream\service.json`:

```json
{
  "controlUrl": "http://127.0.0.1:8081",
  "clientKey": ""
}
```

Если файла нет, используется `http://127.0.0.1:8081`. Для боевого сервера укажите
HTTPS-адрес control API и общий ключ, совпадающий с `TS3STREAM_CLIENT_KEY`.

## Сервер

Стоковый TS3-сервер не меняется. Рядом работают:

- `livekit-server` — SFU, signaling, ICE;
- `ts3stream-control` — выдача JWT, реестр активных стримов, TTL 15 с;
- Caddy/nginx — TLS/WSS и HTTPS для control API.

Готовые шаблоны: `deploy/` (systemd, LiveKit YAML, Caddyfile, `install.sh`).
Подробности — `docs/deployment.md`.

Пошаговые инструкции:

- Linux: [`docs/install-linux.ru.md`](docs/install-linux.ru.md) · [`docs/install-linux.en.md`](docs/install-linux.en.md)
- Windows-сервер: [`docs/install-windows.ru.md`](docs/install-windows.ru.md) · [`docs/install-windows.en.md`](docs/install-windows.en.md)

## Структура

```
plugin/   — C++ плагин TS3 (Qt 5.15.2 x64, LiveKit C++ SDK, WGC, WASAPI)
service/  — Go control API
deploy/   — systemd, LiveKit, Caddy, install.sh
tools/    — сборка, упаковка, вспомогательные утилиты
docs/     — архитектура, звук, развёртывание
dist/     — собранные пакеты (не коммитятся)
```

## Ограничения

- Windows-клиент: только x64, TS3 3.6.2.
- Для системного звука в production должен быть запущен ровно один TS3-клиент; иначе звук отключается (fail closed).
- Захват 4K не включён: 1080p выбран как баланс читаемости текста и нагрузки.
- H.264 hardware encoder зависит от драйвера; при проблемах SDK уходит в software-путь.
- Linux-развёртывание требует домена и TLS-сертификата для WSS/TURN.

## Документы

- `docs/architecture.md` — архитектура и решения.
- `docs/audio.md` — захват звука приложения и устройства.
- `docs/deployment.md` — развёртывание на Linux.
- `docs/sfu-decision.md` — выбор SFU.

