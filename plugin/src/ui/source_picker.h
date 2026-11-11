#pragma once

#include <windows.h>

#include <QString>

#include <optional>
#include <string>

class QWidget;

namespace ts3stream::ui {

struct CaptureSource {
    enum class Type { None, Monitor, Window };
    enum class AudioType { None, Process, Device };

    Type type = Type::None;
    HMONITOR monitor = nullptr;
    HWND window = nullptr;
    QString title;
    DWORD processId = 0;
    QString processName;
    QString processPath;

    // 30/60 are fixed capture ceilings. Auto uses 60 as the ceiling and lets
    // WebRTC reduce the actual frame rate/bitrate when the channel is tight.
    int fps = 30;
    bool autoFps = false;

    AudioType audioType = AudioType::None;
    std::wstring audioDeviceId;
    QString audioDeviceName;
};

std::optional<CaptureSource> pickCaptureSource(QWidget* parent);

} // namespace ts3stream::ui
