#pragma once

#include "wasapi_capture.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ts3stream::media {

// Audio capture hosted in a separate helper process. The helper is required
// for "system audio except TS3": process loopback cannot exclude the calling
// process itself, so the helper (a different process) excludes the TS3 PID.
class RemoteAudioCapture {
public:
    using Callback = std::function<void(const CapturedAudioFrame&)>;

    RemoteAudioCapture();
    ~RemoteAudioCapture();

    RemoteAudioCapture(const RemoteAudioCapture&) = delete;
    RemoteAudioCapture& operator=(const RemoteAudioCapture&) = delete;

    bool startSystemExcludingTs3(DWORD ts3ProcessId, Callback callback);
    bool startApp(DWORD processId, Callback callback);
    void stop();

    bool isRunning() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ts3stream::media
