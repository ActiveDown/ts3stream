# Плагин TS3

C++ x64 DLL для TeamSpeak 3 Client 3.6.2, plugin API 26.

## Планируемые зависимости

- Qt 5.15.2 (MSVC2019 x64) — использовать Qt DLL из папки TS3, не поставлять свои.
- LiveKit C++ SDK (prebuilt Windows x64).
- Windows Graphics Capture (Windows 10 1803+).
- CMake + MSVC v142/v143.

## Структура (план)

```
plugin/
  src/
    plugin.cpp          — entry points TS3
    ui/
      toolbar.cpp       — кнопка в тулбаре
      stream_panel.cpp  — панель просмотра
      fullscreen.cpp    — полноэкранный режим
      client_tree.cpp   — перехват клика по нику
    media/
      capture_wgc.cpp   — захват монитора/окна
      livekit_client.cpp— публикация/приём
      renderer.cpp      — рендер кадров
    ts3/
      ts3_api.cpp       — обёртка над функциями TS3
      server_query.cpp  — иконка LIVE, проверка UID
    config/
    log/
  CMakeLists.txt
```

## Статус

Ожидает установки тулчейна и этапа 0.
