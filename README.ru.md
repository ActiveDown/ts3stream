# TS3Stream

Стриминг экрана и приложений внутри **стокового TeamSpeak 3** на Windows в стиле Discord. LiveKit работает рядом с немодифицированным TS3-сервером.

[English version](README.md)

## Возможности

- **Только стоковый TS3-клиент**: обычный плагин TeamSpeak 3 API 26 (x64) для TS3 3.6.2. Клиент и сервер не патчатся.
- **Кнопка «Стрим»** в тулбаре; активна только при подключении к серверу.
- **Один стрим на клиента**; второй запуск блокируется и в UI, и в коде.
- **30 / 60 / Авто FPS**, захват 1080p, адаптивный H.264 до 12 Мбит/с.
- **Звук приложения или системы** через WASAPI process loopback; звук TeamSpeak исключён, при втором TS3-клиенте звук отключается (fail closed).
- **Комната LiveKit на каждый канал TS3**, красные LIVE-бейджи, встроенный просмотр.
- **Три явных способа смотреть**: контекстное меню клиента, кнопка в панели клиента, popup по LIVE-бейджу. Клик по строке только выделяет клиента.
- **Просмотр и публикация независимы**: старт стрима не открывает viewer, закрытие viewer не останавливает стрим.
- **Fullscreen** на мониторе окна TS3, полупрозрачная панель участников и крупные кнопки.
- **Выход из TS3 без краша**, в том числе во время стрима и просмотра.

## Быстрая установка

### Linux-сервер (автоматический установщик)

Требования:

- Ubuntu 22.04/24.04 или Debian 12, x86_64;
- root/sudo;
- DNS-имя, например `stream.example.com`, указывающее на сервер;
- порты 80/443 открыты для Let's Encrypt;
- **стоковый TS3-сервер уже установлен и запущен** — установщик его не трогает.

Скачайте установщик, посмотрите его и запустите:

```bash
curl -fsSL https://raw.githubusercontent.com/ActiveDown/ts3stream/main/deploy/install-ts3stream.sh -o install-ts3stream.sh
less install-ts3stream.sh
sudo bash install-ts3stream.sh
```

Установщик спросит домен и подтверждение, затем:

- установит LiveKit и `ts3stream-control` API;
- установит Caddy и получит TLS-сертификат;
- создаст systemd-службы `livekit` и `ts3stream`;
- сгенерирует секрет LiveKit API и общий ключ клиента;
- добавит правила UFW для портов стриминга;
- выведет готовый `service.json` для Windows-клиента.

Неинтерактивные примеры:

```bash
# Интернет с автоматическим TLS
sudo bash install-ts3stream.sh --domain stream.example.com --yes

# LAN / тест без TLS (IP разрешён)
sudo bash install-ts3stream.sh --domain 192.168.1.50 --no-caddy --yes
```

Полезные опции: `--version v0.2.1` — зафиксировать релиз control API, `--no-firewall`, `--help`.

После установки:

```bash
systemctl status livekit ts3stream-control caddy
curl https://stream.example.com/api/health
```

Подробные ручные шаги и troubleshooting: [`docs/install-linux.ru.md`](docs/install-linux.ru.md).  
Windows-сервер: [`docs/install-windows.ru.md`](docs/install-windows.ru.md).

### Windows-сервер

Однокомандного установщика для Windows пока нет; пошаговая инструкция покрывает:

- стоковый TS3-сервер на Windows;
- LiveKit для Windows;
- `ts3stream-control.exe`;
- правила Windows Firewall;
- опциональный Caddy TLS и службы через NSSM.

См. [`docs/install-windows.ru.md`](docs/install-windows.ru.md).

### Windows-клиент

1. Закройте TeamSpeak 3.
2. Установите `ts3stream-0.2.1.ts3_plugin` через TeamSpeak Package Installer.
3. Запустите TS3, подключитесь к серверу, нажмите **Стрим**.
4. Создайте `%APPDATA%\TS3Client\plugins\ts3stream\service.json`:

```json
{
  "controlUrl": "https://stream.example.com",
  "clientKey": "PASTE_CLIENT_KEY"
}
```

Установщик выводит точный JSON со сгенерированным ключом.

## Как это работает

```
TS3 client + plugin ──HTTPS/WSS──▶ Caddy ──▶ LiveKit (SFU) + control API
        │                              │
        └── WebRTC media (UDP/TCP) ────▶ LiveKit
```

- Плагин получает JWT в control API, входит в LiveKit-комнату своего канала TS3 и публикует экран и звук.
- Control API хранит секрет LiveKit, выдаёт токены и ведёт реестр активных стримов с TTL 15 секунд для LIVE-бейджей.
- Зритель получает subscribe-only токен и фильтрует выбранного участника по identity.
- Порты TS3-сервера (9987/UDP, 10011/TCP, 30033/TCP) не меняются.

## Сборка из исходников (для разработчиков)

Требования: Windows 10 2004+, Visual Studio 2022 Build Tools (MSVC 14.4x), Windows SDK, Qt 5.15.2 `msvc2019_64`, CMake, Ninja, Go 1.22+.

Скрипты ожидают переносимый тулчейн:

```
%USERPROFILE%\ts3stream-toolchain\
  msvc\
  winsdk\
  Qt\5.15.2\msvc2019_64\
  cmake\...\bin\cmake.exe
  ninja\ninja.exe
```

Другой путь задаётся через `TS3STREAM_TOOLCHAIN`.

```powershell
powershell -File tools\setup-livekit.ps1
powershell -File plugin\build.ps1
powershell -File tools\package.ps1
```

Результат: `dist\ts3stream-<version>.ts3_plugin`.

## Структура

```
plugin/   C++ плагин TS3 (Qt 5.15.2, LiveKit C++ SDK, WGC, WASAPI)
service/  Go control API (токены, реестр live)
deploy/   Linux-установщик, systemd, LiveKit YAML, Caddy
tools/    сборка/упаковка и тестовые утилиты
docs/     установка Linux/Windows, архитектура, звук
```

## Ограничения

- Только Windows x64 и TS3 3.6.2.
- Linux-установщик поддерживает x86_64 Debian/Ubuntu; для других дистрибутивов — ручная инструкция.
- TURN/TLS опционален и требует отдельного сертификата; см. Linux-инструкцию.
- Для системного звука должен быть запущен ровно один TS3-клиент; иначе звук отключается (fail closed).
- Аппаратное H.264-кодирование зависит от драйвера GPU; SDK может уйти в software-путь.

## Лицензия

MIT для оригинального кода. Заголовки TeamSpeak 3 SDK принадлежат TeamSpeak Systems GmbH. LiveKit — Apache-2.0, Qt — LGPL-3.0. Подробности в [NOTICE](NOTICE).
