# Установка TS3Stream на Windows-сервер

Инструкция для Windows-машины, где работают TeamSpeak 3 сервер и серверная
часть TS3Stream. Возможны два варианта:

- **LAN / тест**: control API по `http://`, LiveKit по `ws://`, без TLS. Проще всего.
- **Интернет**: Caddy даёт HTTPS/WSS. Так нужно для реальных пользователей.

Плагин Windows-клиента ставится отдельно; см. раздел 7.

## Быстрая установка (рекомендуется)

Запустите PowerShell **от администратора** и выполните:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "iwr https://raw.githubusercontent.com/ActiveDown/ts3stream/main/deploy/install-ts3stream.ps1 -OutFile $env:TEMP\install-ts3stream.ps1; & $env:TEMP\install-ts3stream.ps1"
```

Установщик поставит LiveKit, control API и опционально Caddy с TLS, запишет
конфигурацию, добавит правила файрвола, создаст задачи автозапуска и проверит
health control API. Стоковый TS3-сервер **не** устанавливается и не меняется;
установщик только проверяет, что он запущен. Если пароль ServerQuery не найден
в `C:\TS3Server\logs`, установщик спросит его; оставьте пустым, чтобы отключить
автонастройку.

LAN/IP без TLS:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "iwr https://raw.githubusercontent.com/ActiveDown/ts3stream/main/deploy/install-ts3stream.ps1 -OutFile $env:TEMP\install-ts3stream.ps1; & $env:TEMP\install-ts3stream.ps1 -Domain 192.168.1.50 -NoTls -Yes"
```

Опции: `-Domain`, `-ApiKey`, `-ApiSecret`, `-ClientKey`, `-Version`, `-NoTls`,
`-NoServices`, `-SkipFirewall`, `-Yes`.

Ручные шаги ниже остаются для полного контроля.

## 1. Подготовка каталогов

Откройте PowerShell от администратора:

```powershell
New-Item -ItemType Directory -Force C:\TS3Server, C:\TS3Stream, C:\TS3Stream\logs | Out-Null
```

Нужно:

- статический LAN IP или публичный IP/DNS;
- открытые порты файрвола;
- последние файлы релиза TS3Stream:
  <https://github.com/ActiveDown/ts3stream/releases>.

## 2. Установка стокового TeamSpeak 3 сервера

1. Скачайте `teamspeak3-server_win64-3.13.9.zip` со страницы
   <https://teamspeak.com/en/downloads/#server>.
2. Распакуйте в `C:\TS3Server`.
3. Запустите один раз из PowerShell:

```powershell
cd C:\TS3Server
.\ts3server.exe license_accepted=1
```

При первом запуске печатается **ключ привилегии ServerAdmin**. Скопируйте его.
Окно можно оставить для первого теста или поставить сервер службой (раздел 9).

Порты TS3 по умолчанию:

- `9987/UDP` — голос;
- `10011/TCP` — ServerQuery;
- `30033/TCP` — передача файлов.

ServerQuery для TS3Stream не нужен, но ключ привилегии нужен, чтобы получить
права администратора в клиенте.

## 3. Установка LiveKit для Windows

1. Скачайте `ts3stream-livekit.exe` из <https://github.com/ActiveDown/ts3stream/releases/tag/v0.3.9>.
2. Сохраните как `C:\TS3Stream\livekit-server.exe`. Серверному буферу нужна именно эта сборка TS3Stream.
3. Сгенерируйте секрет API:

```powershell
-join ((48..57) + (97..102) | Get-Random -Count 64 | ForEach-Object {[char]$_})
```

4. Создайте `C:\TS3Stream\livekit.yaml`:

```yaml
port: 7880

rtc:
  tcp_port: 7881
  udp_port: 7882
  use_external_ip: false

keys:
  ts3stream: PASTE_SECRET

logging:
  level: info
```

Для LAN **не** ставьте `bind_addresses: 127.0.0.1`: клиенты должны подключаться
к `ws://SERVER_IP:7880`. Для TLS за Caddy см. раздел 8.

5. Запустите:

```powershell
cd C:\TS3Stream
.\livekit-server.exe --config .\livekit.yaml
```

Порты `7880/TCP`, `7881/TCP` и `7882/UDP` должны быть доступны клиентам.

## 4. Установка Control API

1. Скачайте `ts3stream-control.exe` из релиза TS3Stream.
2. Положите в `C:\TS3Stream`.
3. Запустите с переменными окружения (замените `SERVER_IP`, секреты и ключи):

```powershell
cd C:\TS3Stream
$env:TS3STREAM_CONTROL_ADDR = "0.0.0.0:8081"
# Серверный буфер A/V 2 с; нужна сборка TS3Stream LiveKit из релиза 0.3.9+.
$env:TS3STREAM_BUFFER_MODE = "server"
$env:TS3STREAM_PLAYOUT_DELAY_MS = "2000"
$env:TS3STREAM_CLIENT_KEY = "PASTE_CLIENT_KEY"
$env:LIVEKIT_API_KEY = "ts3stream"
$env:LIVEKIT_API_SECRET = "PASTE_SECRET"
$env:LIVEKIT_URL = "ws://SERVER_IP:7880"
$env:TS3STREAM_PUBLIC_URL = "http://SERVER_IP:8081"
$env:TS3STREAM_WELCOME_AUTO = "1"
$env:TS3_QUERY_ADDR = "127.0.0.1:10011"
$env:TS3_QUERY_USER = "serveradmin"
$env:TS3_QUERY_PASSWORD = "PASTE_QUERY_PASSWORD"
$env:TS3_QUERY_SID = "1"
.\ts3stream-control.exe
```

`LIVEKIT_URL` возвращается всем TS3-клиентам, поэтому адрес должен быть
реально доступен клиентам. `127.0.0.1` работает только если клиент на этом же ПК.

Блок `TS3_QUERY_*` включает проверку UID и welcome-маркер. Установщик сам ищет
пароль в `C:\TS3Server\logs`; если не нашёл — передайте `-Ts3QueryPassword` или
используйте ручной `service.json`.

4. Проверка:

```powershell
Invoke-RestMethod http://127.0.0.1:8081/api/health
# status : ok
```

## 5. Файрвол

В PowerShell от администратора:

```powershell
New-NetFirewallRule -DisplayName "TS3 Voice UDP" -Direction Inbound -Protocol UDP -LocalPort 9987 -Action Allow
New-NetFirewallRule -DisplayName "TS3 Query TCP" -Direction Inbound -Protocol TCP -LocalPort 10011 -Action Allow
New-NetFirewallRule -DisplayName "TS3 Files TCP" -Direction Inbound -Protocol TCP -LocalPort 30033 -Action Allow
New-NetFirewallRule -DisplayName "TS3Stream LiveKit TCP" -Direction Inbound -Protocol TCP -LocalPort 7880,7881 -Action Allow
New-NetFirewallRule -DisplayName "TS3Stream LiveKit UDP" -Direction Inbound -Protocol UDP -LocalPort 7882 -Action Allow
New-NetFirewallRule -DisplayName "TS3Stream Control TCP" -Direction Inbound -Protocol TCP -LocalPort 8081 -Action Allow
```

Для интернет-сервера не открывайте `8081` напрямую: используйте Caddy и
открывайте только `80/443` плюс медиапорты TS3/LiveKit.

## 6. Проверка с сервера

```powershell
Invoke-RestMethod http://127.0.0.1:8081/api/health
Invoke-RestMethod http://127.0.0.1:8081/api/streams
Get-NetUDPEndpoint -LocalPort 9987
Test-NetConnection 127.0.0.1 -Port 7880
```

## 7. Установка и настройка плагина Windows-клиента

1. Закройте TeamSpeak 3.
2. Установите `ts3stream-0.3.9.ts3_plugin` через TeamSpeak Package Installer.
3. Запустите TS3 и подключитесь к `SERVER_IP:9987`.

Обычно ничего вводить не нужно. Плагин читает welcome-маркер, вызывает
`/api/register`, проверяет TS3 UID через ServerQuery и сам сохраняет
`service.json`.

Если `TS3_QUERY_PASSWORD` не настроен, создайте
`%APPDATA%\TS3Client\plugins\ts3stream\service.json` вручную:

```json
{
  "controlUrl": "http://SERVER_IP:8081",
  "clientKey": "PASTE_CLIENT_KEY"
}
```

Затем перезапустите TS3. Нажмите **Стрим** — кнопка активна только при
подключении. Смотреть стрим можно через контекстное меню клиента, кнопку в
панели клиента или LIVE-бейдж. Запуск своего стрима панель просмотра не открывает.

## 8. TLS через Caddy для интернета (рекомендуется)

Публичный TLS-сертификат требует DNS-имя, а не голый IP. Если своего домена нет,
используйте FreeDNS (<https://freedns.afraid.org/>) или `sslip.io`:
`95-142-44-214.sslip.io` (точки заменяются на дефисы). Создайте A-запись на
публичный IP и проверьте `dig +short yourname.mooo.com` до запуска Caddy.

1. Направьте DNS `stream.example.com` на публичный IP.
2. Скачайте `caddy.exe` для Windows с <https://caddyserver.com/download> и
   положите в `C:\TS3Stream`.
3. Создайте `C:\TS3Stream\Caddyfile`:

```caddyfile
stream.example.com {
    encode zstd gzip
    handle /api/* {
        reverse_proxy 127.0.0.1:8081
    }
    handle /rtc* {
        reverse_proxy 127.0.0.1:7880
    }
    handle /twirp/* {
        reverse_proxy 127.0.0.1:7880
    }
    handle {
        respond "TS3Stream" 200
    }
}
```

4. Переключите LiveKit на localhost и перезапустите:

```yaml
port: 7880
bind_addresses:
  - 127.0.0.1
rtc:
  tcp_port: 7881
  udp_port: 7882
  use_external_ip: true
keys:
  ts3stream: PASTE_SECRET
```

5. Переменные Control API:

```powershell
$env:TS3STREAM_CONTROL_ADDR = "127.0.0.1:8081"
$env:LIVEKIT_URL = "wss://stream.example.com"
```

6. Запустите Caddy:

```powershell
cd C:\TS3Stream
.\caddy.exe run --config .\Caddyfile
```

7. Откройте `80/TCP` и `443/TCP`; закройте `8081/TCP` из интернета.
8. В `service.json` укажите:

```json
{
  "controlUrl": "https://stream.example.com",
  "clientKey": "PASTE_CLIENT_KEY"
}
```

Caddy получит сертификат автоматически и на Windows.

## 9. Службы Windows (опционально)

Удобно через [NSSM](https://nssm.cc/), чтобы серверы переживали выход из
системы и перезагрузку:

```powershell
nssm install TS3Server "C:\TS3Server\ts3server.exe" "license_accepted=1"
nssm set TS3Server AppDirectory "C:\TS3Server"

nssm install TS3StreamLiveKit "C:\TS3Stream\livekit-server.exe" "--config C:\TS3Stream\livekit.yaml"
nssm set TS3StreamLiveKit AppDirectory "C:\TS3Stream"

nssm install TS3StreamControl "C:\TS3Stream\ts3stream-control.exe"
nssm set TS3StreamControl AppDirectory "C:\TS3Stream"
# Те же переменные, что и при ручном запуске:
# nssm set TS3StreamControl AppEnvironmentExtra LIVEKIT_API_KEY=ts3stream LIVEKIT_API_SECRET=... LIVEKIT_URL=ws://SERVER_IP:7880 TS3STREAM_CONTROL_ADDR=0.0.0.0:8081 TS3STREAM_CLIENT_KEY=...

nssm start TS3Server
nssm start TS3StreamLiveKit
nssm start TS3StreamControl
```

## 10. Опционально TURN/TLS

Если клиенты за строгими файрволами, включите TURN в `livekit.yaml` с
сертификатом для `turn.example.com`:

```yaml
turn:
  enabled: true
  domain: turn.example.com
  tls_port: 5349
  udp_port: 3478
  cert_file: C:\TS3Stream\certs\fullchain.pem
  key_file: C:\TS3Stream\certs\privkey.pem
```

Откройте `3478/UDP` и `5349/TCP`. На Windows сертификат должен быть PEM;
сертификаты Let's Encrypt можно скопировать с другого хоста или получить через
win-acme/certbot в WSL.

## 11. Если что-то не работает

- **`/api/health` работает локально, клиент получает 401**: `clientKey` и
  `TS3STREAM_CLIENT_KEY` не совпадают.
- **Токен есть, видео нет**: проверьте `7881/TCP` и `7882/UDP`; для удалённого
  клиента `LIVEKIT_URL` не должен быть `127.0.0.1`.
- **Нет LIVE-бейджа**: `GET /api/streams` должен показывать стрим; TTL 15 секунд.
- **LiveKit не стартует**: синтаксис YAML, несовпадение ключа/секрета, занятый порт.
- **Запросы файрвола Windows**: разрешайте сети осознанно; для интернета
  ограничьте `10011` и `8081` доверенными IP.

