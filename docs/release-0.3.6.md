# TS3Stream 0.3.6

## Русский

- Исправлено зависание трансляции окна Chrome при переключении YouTube в полноэкранный режим. Раньше отправщик отбрасывал кадры с новым размером и повторял последний старый кадр. Теперь существующий видеотрек принимает новые размеры без перепубликации и отключения зрителей.
- Захват WGC пересоздаёт пул кадров при изменении размера окна. Переходный кадр из старого буфера пропускается; копируется только корректная область текстуры.
- В полноэкранном просмотре громкость, mute, панель участников и кнопки «Скрыть»/«Выйти» автоматически скрываются через 3 секунды бездействия. Движение мыши, клик, прокрутка или клавиша возвращают управление. Наведение на панель и перетаскивание ползунка не дают ей скрыться. Ручной выбор видимости участников сохраняется. Обычный и откреплённый неполноэкранный интерфейс остаётся видимым.
- Сохраняется добавленный в 0.3.5 выбор 720p / 1080p / 1440p при запуске стрима; по умолчанию 1080p, без увеличения маленьких окон.

## English

- Fixed frozen Chrome window streams when YouTube enters fullscreen. Previously, frames with new dimensions were discarded and the last old frame was repeated. The existing video track now accepts resized frames without republishing or disconnecting viewers.
- WGC recreates its frame pool when the captured window changes size, discards the old-sized transition frame and copies only a valid texture region.
- Fullscreen playback controls and the members bar auto-hide after three seconds of inactivity. Mouse movement, clicks, scrolling or keyboard input restore them. Hovering controls and dragging the volume slider prevent hiding. The manual members visibility choice is preserved; non-fullscreen controls stay visible.
- Includes the 0.3.5 resolution selector: 720p / 1080p / 1440p, default 1080p, with aspect ratio preserved and no upscaling of smaller windows.

## Verification

- Reproduced the original failure with an isolated-room integration test: stage 1 timed out while the receiver kept getting the old-sized frame.
- The same test now passes eight size/content stages, including 1080p and 1440p, in embedded, detached and fullscreen modes; viewing closes only after the publisher disconnects.
- Native WGC window capture passes fullscreen/restore and resize checks using actual center/corner pixels.
- Auto-hide timer, input wake, control hover, slider dragging, member visibility and mode restoration checks pass.
- Resolution selection, playback gain, LIVE menu snapshot and native multi-monitor window tests pass.

Final Chrome/YouTube confirmation in the installed TS3 client is a separate manual check. Fully exit TS3 before installing the package; publishers need the update for the capture fix, viewers for auto-hide.

Control API binaries are unchanged from 0.3.4 and are included for installer compatibility. Серверные бинарники control API не менялись относительно 0.3.4; они включены для совместимости установщиков.
