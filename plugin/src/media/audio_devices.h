#pragma once

#include <string>
#include <vector>

namespace ts3stream::media {

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

std::vector<AudioDeviceInfo> enumerateRenderDevices();
std::wstring defaultRenderDeviceId();

} // namespace ts3stream::media
