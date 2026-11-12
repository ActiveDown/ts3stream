# Установка TS3Stream на Linux-сервер

Инструкция для Ubuntu 22.04/24.04. Ставим полный стек:

- стоковый TeamSpeak 3 сервер (без патчей);
- LiveKit SFU;
- `ts3stream-control` API;
- Caddy для HTTPS/WSS.

Плагин Windows-клиента ставится отдельно; см. конец инструкции.

## Быстрая установка (рекомендуется)

На чистом Ubuntu 22.04/24.04 или Debian 12 x86_64, где **стоковый TS3-сервер
уже установлен**, выполните:

```bash
curl -fsSL https://raw.githubusercontent.com/ActiveDown/ts3stream/main/deploy/install-linux.sh -o install-linux.sh
less install-linux.sh
sudo bash install-linux.sh
```

Или одной строкой (скачать в `/tmp` и запустить — работает с `sudo`):

```bash
curl -fsSL https://raw.githubusercontent.com/ActiveDown/ts3stream/main/deploy/install-linux.sh -o /tmp/install-linux.sh && sudo bash /tmp/install-linux.sh
```

Установщик спросит публичное DNS-имя и подтверждение, затем поставит LiveKit,
control API и Caddy, создаст systemd-службы, сгенерирует секреты, добавит
правила UFW и выведет готовый `service.json` для Windows-клиентов.

Если пароль ServerQuery не найден в логах TS3, установщик спросит его и
предложит автоматический сброс (TS3-сервер будет перезапущен). Для
неинтерактивной установки используйте `--ts3-query-password '...'` или
`--reset-ts3-query`.

Перед настройкой Caddy установщик проверяет, что домен указывает на публичный
IP этого сервера, и предупреждает, если это не так. Если вы используете
FreeDNS/no-IP, сначала обновите A-запись или передайте URL динамического
обновления через `--ddns-update-url '...'`.

Неинтерактивные примеры:

```bash
# Интернет с автоматическим TLS
sudo bash install-linux.sh --domain stream.example.com --yes

# LAN / тест без TLS
sudo bash install-linux.sh --domain 192.168.1.50 --no-caddy --yes
```

Есть только публичный IP, без домена:

```bash
# HTTP/WS, без TLS
sudo bash install-linux.sh --domain 95.142.44.214 --no-caddy --yes

# Бесплатный DNS + TLS: sslip.io резолвит 95-142-44-214.sslip.io в 95.142.44.214
sudo bash install-linux.sh --domain 95-142-44-214.sslip.io --yes
```

`sslip.io` — бесплатный wildcard DNS; порты 80/443 должны быть доступны, чтобы
Caddy получил сертификат Let's Encrypt. Можно также использовать DuckDNS или
любое своё DNS-имя.

FreeDNS (<https://freedns.afraid.org/>) тоже подойдёт:

1. Зарегистрируйте бесплатный аккаунт и создайте поддомен на одном из общих доменов.
2. Добавьте **A-запись** с адресом `95.142.44.214` (ваш публичный IP).
3. Проверьте: `dig +short yourname.mooo.com` — должен вернуться этот IP.
4. Запустите установщик с `--domain yourname.mooo.com`.

Одного сертификата на поддомен достаточно. На общих доменах FreeDNS действуют
обычные лимиты Let's Encrypt; перед A-записью не должно быть прокси/CDN, если он
не проксирует одновременно WebSocket и UDP-медиа.

Если нужен полный контроль или у вас другой дистрибутив — используйте ручные
шаги ниже.

## Ручная установка

## 1. Подготовка сервера

Нужно:

- Linux VPS с публичным IPv4;
- DNS-имя, например `stream.example.com`, указывающее на VPS;
- root/sudo;
- порты 80/443 доступны для Let's Encrypt.

Создайте пользователей и каталоги:

```bash
sudo useradd --system --home /opt/ts3server --shell /usr/sbin/nologin ts3 || true
sudo useradd --system --home /opt/ts3stream --shell /usr/sbin/nologin livekit || true
sudo useradd --system --home /opt/ts3stream --shell /usr/sbin/nologin ts3stream || true
sudo mkdir -p /opt/ts3server /opt/ts3stream/bin /etc/ts3stream /var/log/ts3stream
```

## 2. Установка стокового TeamSpeak 3 сервера

```bash
cd /tmp
curl -LO https://files.teamspeak-services.com/releases/server/3.13.9/teamspeak3-server_linux_amd64-3.13.9.tar.bz2
tar -xjf teamspeak3-server_linux_amd64-3.13.9.tar.bz2
sudo cp -a teamspeak3-server_linux_amd64/. /opt/ts3server/
sudo chown -R ts3:ts3 /opt/ts3server
```

Создайте `/etc/systemd/system/ts3server.service`:

```ini
[Unit]
Description=TeamSpeak 3 Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=ts3
Group=ts3
WorkingDirectory=/opt/ts3server
ExecStart=/opt/ts3server/ts3server license_accepted=1
Restart=on-failure
RestartSec=3
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

Запустите:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ts3server
```

При первом запуске TS3 печатает **ключ привилегии ServerAdmin** и пароль
ServerQuery. Скопируйте их:

```bash
sudo journalctl -u ts3server -n 100 --no-pager
# или:
sudo grep -R "token=" /opt/ts3server/logs | tail
```

ServerQuery для TS3Stream не нужен, но ключ привилегии сохраните: это
единственный способ получить права администратора в TS3-клиенте.

## 3. Установка LiveKit

Скачайте последний Linux amd64 архив со страницы
<https://github.com/livekit/livekit/releases> и распакуйте `livekit-server`.
Пример для v1.13.11:

```bash
cd /tmp
curl -L -o livekit.tar.gz \
  https://github.com/livekit/livekit/releases/download/v1.13.11/livekit_1.13.11_linux_amd64.tar.gz
tar -xzf livekit.tar.gz
sudo install -m 0755 livekit-server /opt/ts3stream/bin/livekit-server
```

Сгенерируйте секрет API:

```bash
openssl rand -hex 32
```

Создайте `/etc/ts3stream/livekit.yaml` (замените `PASTE_SECRET`):

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

logging:
  level: info
```

Важно:

- `7880` — signaling, слушает только localhost за Caddy;
- `7881/TCP` и `7882/UDP` — медиа, должны быть доступны клиентам;
- TURN в минимальном конфиге выключен, см. раздел 8.

Создайте `/etc/systemd/system/livekit.service`:

```ini
[Unit]
Description=LiveKit Server for TS3Stream
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=livekit
Group=livekit
ExecStart=/opt/ts3stream/bin/livekit-server --config /etc/ts3stream/livekit.yaml
Restart=on-failure
RestartSec=3
LimitNOFILE=65536
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

```bash
sudo chown -R livekit:livekit /etc/ts3stream/livekit.yaml
sudo systemctl daemon-reload
sudo systemctl enable --now livekit
```

## 4. Установка Control API

Скачайте `ts3stream-control-linux-amd64` из релиза TS3Stream:

```bash
sudo curl -L -o /opt/ts3stream/bin/ts3stream-control \
  https://github.com/ActiveDown/ts3stream/releases/download/v0.3.4/ts3stream-control-linux-amd64
sudo chmod +x /opt/ts3stream/bin/ts3stream-control
```

Создайте `/etc/ts3stream/control.env` (замените оба секрета):

```bash
TS3STREAM_CONTROL_ADDR=127.0.0.1:8081
TS3STREAM_CLIENT_KEY=PASTE_CLIENT_KEY
LIVEKIT_API_KEY=ts3stream
LIVEKIT_API_SECRET=PASTE_SECRET
LIVEKIT_URL=wss://stream.example.com

# Автонастройка / проверка UID (установщик заполняет сам)
TS3STREAM_PUBLIC_URL=https://stream.example.com
TS3STREAM_WELCOME_AUTO=1
TS3_QUERY_ADDR=127.0.0.1:10011
TS3_QUERY_USER=serveradmin
TS3_QUERY_PASSWORD=PASTE_QUERY_PASSWORD
TS3_QUERY_SID=1
```

`LIVEKIT_URL` возвращается Windows-плагину, поэтому это должен быть публичный
WSS-адрес, а не `127.0.0.1`.

Блок `TS3_QUERY_*` включает `/api/register` и запись welcome-маркера.
Установщик сам находит пароль ServerQuery в логах первого запуска TS3. Если
пароля нет, клиенты могут использовать ручной `service.json`; точная ошибка
query будет в логе control API.

Создайте `/etc/systemd/system/ts3stream-control.service`:

```ini
[Unit]
Description=TS3Stream control API
After=network-online.target livekit.service
Wants=network-online.target

[Service]
Type=simple
User=ts3stream
Group=ts3stream
EnvironmentFile=/etc/ts3stream/control.env
ExecStart=/opt/ts3stream/bin/ts3stream-control
Restart=on-failure
RestartSec=3
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

```bash
sudo chown -R ts3stream:ts3stream /etc/ts3stream/control.env /var/log/ts3stream
sudo systemctl daemon-reload
sudo systemctl enable --now ts3stream-control
```

## 5. Caddy и TLS

```bash
sudo apt install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
  | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
  | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt update
sudo apt install -y caddy
```

Замените `/etc/caddy/Caddyfile`:

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

```bash
sudo caddy validate --config /etc/caddy/Caddyfile
sudo systemctl restart caddy
curl https://stream.example.com/api/health
# {"status":"ok"}
```

Caddy сам получит и будет обновлять TLS-сертификат.

## 6. Файрвол

Для UFW:

```bash
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw allow 9987/udp
sudo ufw allow 10011/tcp
sudo ufw allow 30033/tcp
sudo ufw allow 7881/tcp
sudo ufw allow 7882/udp
# только если включаете TURN:
sudo ufw allow 3478/udp
sudo ufw allow 5349/tcp
sudo ufw enable
```

Не открывайте `7880` и `8081` наружу: Caddy проксирует их с localhost.

## 7. Установка и настройка Windows-клиента

1. Установите `ts3stream-0.3.4.ts3_plugin` через TeamSpeak Package Installer.
2. Запустите TS3 и подключитесь к серверу.

Обычно ничего вводить не нужно. Плагин читает маркер
`[ts3stream]https://...[/ts3stream]` из welcome message сервера, вызывает
`/api/register`, проверяет TS3 UID через ServerQuery и сам сохраняет
`service.json`.

Если автонастройка отключена (установщик не нашёл пароль ServerQuery), создайте
`%APPDATA%\TS3Client\plugins\ts3stream\service.json` вручную:

```json
{
  "controlUrl": "https://stream.example.com",
  "clientKey": "PASTE_CLIENT_KEY"
}
```

Затем перезапустите TS3. Кнопка **Стрим** активна только при подключении.
Запустите стрим и откройте просмотр через контекстное меню клиента, кнопку в
панели клиента или LIVE-бейдж.

## 8. Опционально: TURN/TLS

TURN помогает клиентам за строгими файрволами.

1. Направьте `turn.example.com` на этот же сервер.
2. Получите сертификат:

```bash
sudo apt install -y certbot
sudo systemctl stop caddy
sudo certbot certonly --standalone -d turn.example.com
sudo systemctl start caddy
```

3. Скопируйте сертификат туда, где пользователь LiveKit сможет его прочитать,
   и добавьте TURN в `/etc/ts3stream/livekit.yaml`:

```bash
sudo mkdir -p /etc/ts3stream/certs
sudo cp /etc/letsencrypt/live/turn.example.com/fullchain.pem /etc/ts3stream/certs/
sudo cp /etc/letsencrypt/live/turn.example.com/privkey.pem /etc/ts3stream/certs/
sudo chown -R livekit:livekit /etc/ts3stream/certs
sudo chmod 640 /etc/ts3stream/certs/*
```

```yaml
turn:
  enabled: true
  domain: turn.example.com
  tls_port: 5349
  udp_port: 3478
  cert_file: /etc/ts3stream/certs/fullchain.pem
  key_file: /etc/ts3stream/certs/privkey.pem
```

4. Перезапустите LiveKit:

```bash
sudo systemctl restart livekit
```

5. Откройте `3478/udp` и `5349/tcp`.

6. После обновления сертификата скопируйте новые `fullchain.pem` и
   `privkey.pem` в `/etc/ts3stream/certs/` и перезапустите LiveKit. Это можно
   автоматизировать deploy-hook'ом certbot.

## 9. Проверка

```bash
sudo systemctl status ts3server livekit ts3stream-control caddy
curl https://stream.example.com/api/health
curl https://stream.example.com/api/streams
sudo journalctl -u ts3stream-control -n 50 --no-pager
```

В TS3-клиенте:

- подключитесь к `stream.example.com:9987` (или IP сервера);
- нажмите **Стрим**;
- у других клиентов в канале должен появиться LIVE-бейдж;
- встроенный просмотр должен получать видео 1080p.

## 10. Обновление

```bash
sudo systemctl stop ts3stream-control
sudo curl -L -o /opt/ts3stream/bin/ts3stream-control \
  https://github.com/ActiveDown/ts3stream/releases/latest/download/ts3stream-control-linux-amd64
sudo chmod +x /opt/ts3stream/bin/ts3stream-control
sudo systemctl start ts3stream-control
```

Плагин на Windows обновляется новым `.ts3_plugin`.

## 11. Если что-то не работает

- **401 от control API**: `clientKey` в `service.json` не совпадает с
  `TS3STREAM_CLIENT_KEY`.
- **Токен есть, видео нет**: откройте `7881/TCP` и `7882/UDP`; проверьте
  `use_external_ip` и файрвол хостинга.
- **Нет LIVE-бейджа**: проверьте `curl https://.../api/streams` и логи
  control API; TTL реестра — 15 секунд.
- **Ошибка сертификата Caddy**: DNS должен указывать на сервер, порт 80 должен
  быть доступен.
- **LiveKit не стартует**: проверьте YAML и совпадение секрета с `control.env`.




