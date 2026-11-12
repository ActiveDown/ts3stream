# Buffered A/V playback (0.3.8+)

## Русский

**Новая установка Linux/Windows автоматически включает 2000 мс для видео и звука.** Все зрители должны использовать плагин 0.3.8 или новее. Повторный запуск основного Linux- или Windows-установщика сохраняет прежнюю настройку; старый конфиг без параметра означает 0 мс, чтобы не нарушить просмотр на старых клиентах.

- Linux: `install-linux.sh --playout-delay-ms 2000` (либо `0` для отключения).
- Windows: `install-ts3stream.ps1 -PlayoutDelayMs 2000` (либо `0`).
- Настройка control API: `TS3STREAM_PLAYOUT_DELAY_MS=2000`. Без переменной сам бинарник сохраняет прежнее поведение — 0 мс. Допустимы значения 0–5000 с шагом 10 мс; проверенный режим — 2000 мс.

### Как устроено

Control API подписывает JWT с `RoomConfiguration`: одинаковые `min_playout_delay`/`max_playout_delay`, `sync_streams=true` и маркер `ts3stream-av-delay-v1:2000`. LiveKit применяет это при **создании** комнаты. Клиент читает метаданные **фактической комнаты**, а не только пожелания токена: если комната уже существовала без буфера, звук тоже остаётся без добавленной задержки.

Видео буферизуется внутри WebRTC. Сырые аудиоколбэки C++ SDK не проходят ту же A/V-синхронизацию, поэтому плагин задерживает PCM на согласованный интервал по абсолютным монотонным срокам. В плагине нет многосекундной очереди RGBA-видеокадров. Очередь PCM при 2000 мс ограничена примерно 480 КБ (плюс служебные структуры); при разрыве, переподключении, смене источника и закрытии старый звук сбрасывается. Громкость/mute применяются при выводе, поэтому не ждут две секунды.

В панели появляется `буфер A/V 2,0 с`, в диагностике — `LiveKit room audio compensation=2000 ms`. Набор буфера и старт кодировщика могут занять больше двух секунд; краткие паузы/сбросы во время начального заполнения возможны. После переподключения буфер набирается заново. Завершение стрима/закрытие просмотра прерывает и отбрасывает буферизированный хвост, а не воспроизводит старый звук после закрытия.

### Обновление существующего сервера

1. Сначала обновить плагины зрителей до 0.3.8+; автору также рекомендуется обновление ради 10 Мбит/с и исправлений захвата.
2. Заменить control API на бинарник из релиза 0.3.8 и задать `TS3STREAM_PLAYOUT_DELAY_MS=2000` в его окружении (`/etc/ts3stream/control.env` на Linux либо `start-control.cmd` на Windows).
3. Перезапустить **только control API**. LiveKit и TS3 для этого перезапускать не требуется. Не использовать полный серверный установщик ради одного обновления, если нужно сохранить текущую трансляцию.
4. Дождаться закрытия старой комнаты после выхода всех участников/зрителей, затем создать новую трансляцией. Активные комнаты намеренно не перенастраиваются; не удалять их принудительно во время просмотра.

Не включать отдельно глобальный `room.playout_delay` в `livekit.yaml`: без соответствующего маркера новый клиент не узнает, что нужно задерживать звук. При откате использовать `0` и дождаться создания новых комнат.

### Проверки и пределы

Тест с шестью 1080p сценами и различимыми по частоте звуковыми импульсами проверяет задержку обоих выходных колбэков и их разницу менее 150 мс. Также проверены позднее подключение, новое подключение существующим объектом viewer, сохранение режима старой комнаты, ограничение памяти и прерываемое закрытие. В локальных прогонах с 2000 мс разница медиан аудио/видео составила примерно 1–31 мс. Это **измерение до панели и аудиоустройства**, не гарантия точности на физических экране/колонках или при любой перегрузке сети. Устройство добавляет свою небольшую задержку. Длительные потери пакетов, перегрузка кодировщика и зависание самого источника буфер не устраняет.

## English

Fresh Linux/Windows installations configure **2000 ms** by default. Viewers require plugin **0.3.8+**. The main Linux and Windows installers preserve existing settings on reinstall; a legacy config without this field remains unbuffered. Overrides: `--playout-delay-ms 0` / `-PlayoutDelayMs 0` (disable), or `2000` (enable).

The control API reads `TS3STREAM_PLAYOUT_DELAY_MS` (standalone default: 0) and signs a room configuration containing matching video playout bounds, `sync_streams`, and an audio-delay metadata marker. The plugin reads the **actual room** metadata and delays raw PCM using bounded, monotonic-deadline scheduling. Existing rooms keep their old mode, preventing audio-only delay when a newer token joins an older room. Video buffering stays inside WebRTC; there is no large RGBA frame queue in the plugin.

For an existing deployment, update viewers first, install the new control API binary, set the environment variable and restart only the control API. Wait for the old room to close naturally before creating a new one. Do not separately enable global LiveKit playout delay without the matching metadata contract. Startup/reconnection require buffering; stopping viewing immediately discards queued audio. Local callback-level A/V tests pass, but physical-device latency and arbitrary WAN congestion remain separate real-world checks.
