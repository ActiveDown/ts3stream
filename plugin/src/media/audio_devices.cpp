#include "audio_devices.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <winrt/base.h>

#include <algorithm>

namespace ts3stream::media {
namespace {

class ComApartment {
public:
    ComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(hr);
    }
    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

private:
    bool initialized_ = false;
};

std::wstring deviceId(IMMDevice* device) {
    if (!device) {
        return {};
    }
    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        result = id;
        CoTaskMemFree(id);
    }
    return result;
}

std::wstring deviceName(IMMDevice* device) {
    if (!device) {
        return {};
    }

    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) {
        return {};
    }

    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        result = value.pwszVal;
    }
    PropVariantClear(&value);
    return result;
}

} // namespace

std::wstring defaultRenderDeviceId() {
    ComApartment com;
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.put())))) {
        return {};
    }

    winrt::com_ptr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) {
        return {};
    }
    return deviceId(device.get());
}

std::vector<AudioDeviceInfo> enumerateRenderDevices() {
    ComApartment com;
    std::vector<AudioDeviceInfo> result;

    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.put())))) {
        return result;
    }

    winrt::com_ptr<IMMDevice> defaultDevice;
    std::wstring defaultId;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, defaultDevice.put()))) {
        defaultId = deviceId(defaultDevice.get());
    }

    winrt::com_ptr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put()))) {
        return result;
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return result;
    }

    for (UINT i = 0; i < count; ++i) {
        winrt::com_ptr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) {
            continue;
        }

        AudioDeviceInfo info;
        info.id = deviceId(device.get());
        info.name = deviceName(device.get());
        info.isDefault = !defaultId.empty() && info.id == defaultId;
        if (info.name.empty()) {
            info.name = info.isDefault ? L"Устройство по умолчанию" : L"Аудиоустройство";
        }
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
        if (a.isDefault != b.isDefault) {
            return a.isDefault > b.isDefault;
        }
        return a.name < b.name;
    });

    return result;
}

} // namespace ts3stream::media
