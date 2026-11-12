# TS3Stream 0.3.3

## Русский

- Исправлено «Ожидание кадра» у участников с `+` в TeamSpeak UID: control API сохраняет ID при выдаче токена LiveKit. Совместимо с плагином 0.3.2.
- Исправлен переход в полный экран на другой монитор после отделения панели от TS3.
- Добавлены кнопки **«Открепить» / «Встроить»**. Отдельное окно можно перенести на другой экран; TS3 остаётся доступен. Fullscreen открывается на мониторе самого окна просмотра и возвращает его прежнее положение/размер.
- Просмотр автоматически закрывается при уходе автора из LiveKit или снятии публикации видео. Список активных стримов служит резервной проверкой; ошибки HTTP и устаревшие ответы не закрывают новый просмотр.
- Закрытие отдельного окна крестиком или Alt+F4 прекращает просмотр и звук, но не собственную публикацию пользователя.

### Обновление

1. Полностью закройте TS3 и установите `ts3stream-0.3.3.ts3_plugin`.
2. Обновите control API на сервере файлом для своей ОС (или повторно запустите установщик с версией `v0.3.3`).
3. После обновления control API перезапустите уже идущие стримы: старые токены могли содержать испорченный ID.

## English

- Fixed missing video for TeamSpeak UIDs containing `+`: the control API preserves the identity in LiveKit tokens, including requests from plugin 0.3.2.
- Fixed fullscreen jumping to another monitor after reparenting the embedded viewer.
- Added **Detach / Embed** controls (Russian UI labels: «Открепить» / «Встроить»). Move the independent viewer to another screen while keeping TS3 accessible. Fullscreen uses the viewer's monitor and restores its previous window placement.
- Automatically close playback when the publisher leaves LiveKit or unpublishes video. Successful active-stream snapshots provide a fallback; HTTP errors and stale responses cannot close a newer viewer.
- Native window close / Alt+F4 stops playback and audio without stopping the user's own publication.

### Upgrade

1. Fully exit TS3 and install `ts3stream-0.3.3.ts3_plugin`.
2. Update the server control API binary for your OS, or rerun the installer with version `v0.3.3`.
3. Restart existing streams after updating the control API to replace any tokens with corrupted identities.

## Verification / Проверки

- `go test ./...`, `go vet ./...` in `service/`; regression tests cover raw/encoded `+`, `/`, `=`, viewer suffixes, leading/trailing `+`, spaces and percent decoding.
- Native Qt/Win32 test: nine cycles on three physical monitors, including portrait/negative coordinates, embedded and detached fullscreen, and restoration of normal/maximized windows.
- LiveKit integration test: received video then stopped the publisher in embedded, detached and fullscreen modes. Each viewer received exactly one end callback and closed; another participant leaving did not close it.
- Full plugin build and Linux/Windows control API builds.
