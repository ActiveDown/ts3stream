# TS3Stream 0.3.11 — сохранение качества и 900p

- **Битрейт — выпадающий список**: 1000–10000 Кбит/с с интервалом 1000. Можно ввести своё значение, например 5500.
- **Разрешение, FPS и битрейт сохраняются** сразу после успешного запуска захвата и подключения LiveKit. При следующем запуске стрима и после перезапуска TeamSpeak подставляются сохранённые значения. «60 FPS» и «Авто» различаются.
- Добавлено **900p (1600×900)** для окна и монитора. Пропорции сохраняются, маленькие источники не увеличиваются.
- Громкость просмотра теперь **0–150%**, исходный уровень — 100%. Усиливается только звук просматриваемого стрима, не голоса TeamSpeak; пики PCM ограничиваются без переполнения.
- Настройки хранятся локально в `%APPDATA%\TS3Client\plugins\ts3stream\stream-settings.json`. Отмена окна и ошибка запуска не перезаписывают настройки. Запись атомарная; повреждённые значения заменяются начальными.
- Первый запуск без сохранённых настроек: **1080p, 30 FPS, 4000 Кбит/с**.

## Установка

Закройте TeamSpeak, установите `ts3stream-0.3.11.ts3_plugin` штатным установщиком и запустите TeamSpeak снова. Пользовательский файл настроек не включён в пакет и не заменяется обновлением.

Это клиентское обновление. **Сервер с 0.3.10 обновлять или перезапускать не нужно.** Серверные бинарники в комплекте те же: LiveKit `1.13.11-ts3stream.4` и прежний control API. Проверочные суммы — `SHA256SUMS.txt`.

Изменения не добавляют клиентский буфер и не меняют сетевой транспорт. Полное устранение WAN-зависаний этим выпуском не заявляется.

## Проверки

Собраны плагин и аудиопомощник. Прошли 10 клиентских тестов: окно выбора и масштабирование, битрейт, управление просмотром/PCM-усиление, автоскрытие, очередь видео, RTC-диагностика, задержка и очередь аудио, композиция рабочего стола, меню стримов. Сохранение проверено на временном профиле с открытием окна в новых процессах для 30/60/Авто, 900p и произвольного битрейта; также проверены отмена, повреждённые настройки и ошибка записи. Реальный профиль пользователя тестами не изменялся.

## English

- Editable bitrate dropdown: 1000–10000 Kbit/s presets, custom values supported.
- Resolution, FPS mode (including distinct Auto/60) and bitrate are saved atomically after capture starts successfully with LiveKit connected, then restored across stream and TeamSpeak restarts. Cancelled/failed starts leave saved preferences unchanged.
- New **900p (1600×900)** option for monitor and window capture, preserving aspect ratio without upscaling.
- Viewer audio gain up to **150%**, with a 100% default and saturating PCM peaks; TeamSpeak voices and outgoing audio are unaffected.
- Local preferences: `%APPDATA%\TS3Client\plugins\ts3stream\stream-settings.json`. First-run defaults remain 1080p, 30 FPS, 4000 Kbit/s.
- Install the plugin package with TeamSpeak closed. No server update/restart is needed over 0.3.10; bundled server binaries are unchanged. Remaining WAN freezes are not claimed fixed.
