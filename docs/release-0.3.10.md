# TS3Stream 0.3.10 — выбор битрейта, по умолчанию 4000 Кбит/с

## Клиент

- Поле «Битрейт видео» внизу окна «Начать стрим»: 1000–10000 Кбит/с, шаг кнопок 1000, ввод с клавиатуры.
- Новый стандартный потолок — **4000 Кбит/с (4 Мбит/с)** вместо 10 Мбит/с.
- Выбор применяется к видеокодировщику при старте; адаптация WebRTC при ухудшении связи сохраняется. Звук и повторные передачи добавляют трафик сверх видеобитрейта.
- Настройка клиентская: ввод битрейта в серверный установщик не добавляется. Дополнительная двухсекундная клиентская задержка не добавляется.

## Сервер

В комплекте `1.13.11-ts3stream.4`: реальная ёмкость кеша 16384 видео- / 2048 аудиопакетов, упорядочивание задержанных RTP-пакетов и повторный выбор после ожидания, диагностика причин потерь. Эти исправления ранее проверялись и устанавливались отдельно; теперь исходники, патч и бинарники входят в один публичный выпуск. По пересылке версия эквивалентна `1.13.11-ts3stream.3-diagnostic`.

Для серверов с исходным SFU из 0.3.9 требуется заменить LiveKit и перезапустить только его. TeamSpeak-сервер и control API для этих изменений перезапускать не требуется. Плагин 0.3.10 нужен автору для нового поля битрейта.

## Важно

Это **не заявление о полном устранении рывков**. В реальных WAN-прогонах оставались синхронные остановки и при 5 Мбит/с; точный механизм невосстановленных входящих пакетов ещё исследуется. Снижение потолка может уменьшать нагрузку, но не гарантирует гладкое воспроизведение на любой сети. Подробнее: [измерения](wan-capture-2026-11-12.md).

## Установка

Скачайте `ts3stream-0.3.10.ts3_plugin`, полностью закройте TeamSpeak и установите пакет штатным установщиком TeamSpeak. Серверные файлы: `ts3stream-livekit-linux-amd64`, `ts3stream-livekit.exe`, `ts3stream-control-linux-amd64`, `ts3stream-control.exe`. Контрольные суммы — `SHA256SUMS.txt`.

## Проверки сборки

- Сборка плагина/helper и 11 клиентских проверок: выбор/ввод битрейта, границы/шаг/отмена, преобразование единиц/приоритет, очередь отправки, RTC-диагностика, аудиозадержка и очередь устройства, композиция рабочего стола, viewer, автоскрытие, меню и визуальные метки.
- Control API: `go test ./...`, `go vet ./...`, Windows/Linux amd64.
- Патч SFU применён к чистому закреплённому upstream; прошли `TestServerAV`, `go vet`, полный Linux `pkg/sfu/buffer` и Linux `pkg/sfu` в short-режиме. Собраны Windows/Linux amd64.
- Локальная A/V-интеграция с control API после 60 секунд прогрева: видео **2072,1 мс**, звук **2060,7 мс**, разница **−11,4 мс**, 6/6 сцен и 5/5 звуковых импульсов. Это callback-измерение, не физическая задержка экрана/динамиков и не гарантия WAN-плавности.
- Проверены состав/CRC пакета, совпадение вложенных бинарников со сборкой, экспортируемая DLL версия 0.3.10 и SHA256 файлов.

---

## English

Client-side video bitrate selection: 1000–10000 Kbit/s, 1000 Kbit/s button step, **4000 Kbit/s default**. The selected encoder ceiling takes precedence over the legacy diagnostic environment override; WebRTC congestion adaptation remains enabled. No new installer setting or extra client-side two-second buffer.

Includes LiveKit fork `1.13.11-ts3stream.4`: full initial packet-cache allocation, sequence-ordered server buffering, post-wait reselection, and diagnostic counters. Forwarding is equivalent to the previously deployed revision 3 diagnostic build. **Remaining WAN freezes are not claimed fixed.** See the linked measurement report. Install the plugin package after closing TeamSpeak; update only LiveKit where an older SFU is deployed.
