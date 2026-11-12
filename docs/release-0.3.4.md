# TS3Stream 0.3.4

## Русский

- **Исправлено падение при выборе стрима через LIVE.** Меню больше не хранит итератор обновляемого списка; данные выбора копируются, вложенный цикл событий убран. Аргументы открытия также передаются по значению. Повторный просмотр того же стрима поднимает существующее окно без переподключения.
- **Исправлено долгое ожидание первого кадра для неподвижных окон.** Последний кадр периодически подаётся в кодировщик, чтобы запрос ключевого кадра от нового зрителя выполнялся без перерисовки захватываемого окна. Частота обновления при простое — до 4 кадров/с.
- **Mute и громкость 0–100%.** Управление звуком просмотра доступно внутри TS3, в откреплённом окне и fullscreen, даже при скрытой панели участников. Настройки сохраняются при переключении режимов/стримов в текущем сеансе TS3. Меняется только принимаемый PCM-звук стрима, не системный микшер и не исходящий звук.
- **«Идущие стримы (N)»** рядом со «Стрим»: меню трансляций на текущем сервере с ником и номером канала. Кнопка скрывается, если стримов нет или соединение отключено.
- **LIVE-бейдж** привязан к видимой границе списка, а не к ширине скрытой части колонки. Область клика совпадает с отрисовкой.

### Обновление

Полностью закройте TS3 и установите `ts3stream-0.3.4.ts3_plugin`. Обновиться нужно и автору трансляции, чтобы исправление первого кадра работало у его зрителей. Control API не менялся относительно 0.3.3; бинарники включены для совместимости установщиков.

## English

- Fixed the LIVE-menu crash: menu actions own stream snapshots rather than invalidatable registry iterators, opening arguments are passed by value, and the nested menu event loop is removed. Reopening the same stream raises the existing viewer without reconnecting.
- Fixed delayed first video for late subscribers to static windows. The sender feeds its cached frame to the encoder at up to 4 FPS while idle, allowing keyframe requests to complete without a source repaint.
- Added playback mute and 0–100% volume in embedded, detached and fullscreen modes, including with the members bar hidden. Settings survive mode/stream switches within the TS3 session. Gain is applied only to received stream PCM, never the system mixer or outgoing capture.
- Added an active-streams toolbar menu for the current TS3 server, with nickname/channel labels and a count. It hides when no streams are available or the client disconnects.
- LIVE badges and their hitboxes now follow the visible viewport edge when the client list is narrowed or scrolled.

### Upgrade

Fully exit TS3 and install `ts3stream-0.3.4.ts3_plugin`. Publishers must upgrade too for their viewers to benefit from the first-frame fix. Control API binaries are unchanged from 0.3.3 and are included for installer compatibility.

## Verification / Проверки

- Inspected the 15:32 client dump: original exception in `QString::operator=` from the plugin mouse-event path, followed by fail-fast during process teardown.
- Regression test: replace the entire stream registry while its popup stays open, then select the original stream.
- Late-subscription integration test reproduced zero received frames before the fix; after the fix it passes with a single static source frame in all three viewer modes. Continuous video and stream-ended tests pass too.
- PCM gain/control tests, narrowed LIVE geometry, active-stream menu refresh/visibility tests.
- Nine native fullscreen/detached/maximized restoration cycles on three monitors.
