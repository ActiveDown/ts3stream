#include "wasapi_capture.h"

#include "../log.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

#include <winrt/base.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace ts3stream::media {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;
constexpr int kBlockAlign = kChannels * kBitsPerSample / 8;

std::string hresultText(HRESULT hr) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

class ActivationHandler
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::FtmBase,
          IActivateAudioInterfaceCompletionHandler> {
public:
    ActivationHandler() {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~ActivationHandler() override {
        if (event_) {
            CloseHandle(event_);
        }
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        winrt::com_ptr<IUnknown> audioInterface;
        HRESULT activateResult = E_FAIL;
        const HRESULT hr = operation->GetActivateResult(&activateResult, audioInterface.put());
        if (SUCCEEDED(hr) && SUCCEEDED(activateResult) && audioInterface) {
            if (SUCCEEDED(audioInterface->QueryInterface(__uuidof(IAudioClient), client.put_void()))) {
                result = S_OK;
            } else {
                result = E_NOINTERFACE;
            }
        } else {
            result = FAILED(hr) ? hr : activateResult;
        }

        if (event_) {
            SetEvent(event_);
        }
        return S_OK;
    }

    HANDLE event() const {
        return event_;
    }

    HRESULT result = E_FAIL;
    winrt::com_ptr<IAudioClient> client;

private:
    HANDLE event_ = nullptr;
};

} // namespace

struct WasapiCapture::Impl {
    std::atomic<bool> running{false};
    Callback callback;
    std::string error;

    winrt::com_ptr<IAudioClient> audioClient;
    winrt::com_ptr<IAudioCaptureClient> captureClient;
    HANDLE sampleEvent = nullptr;
    std::thread thread;
    int64_t startTimeUs = 0;

    ~Impl() {
        stop();
    }

    bool initializeClient(winrt::com_ptr<IAudioClient> client) {
        audioClient = std::move(client);
        if (!audioClient) {
            error = "Audio client is null";
            return false;
        }

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = kChannels;
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = kBitsPerSample;
        format.nBlockAlign = kBlockAlign;
        format.nAvgBytesPerSec = kSampleRate * kBlockAlign;
        format.cbSize = 0;

        HRESULT hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            0,
            0,
            &format,
            nullptr);
        if (FAILED(hr)) {
            error = "IAudioClient::Initialize failed: " + hresultText(hr);
            return false;
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), captureClient.put_void());
        if (FAILED(hr)) {
            error = "GetService(IAudioCaptureClient) failed: " + hresultText(hr);
            return false;
        }

        sampleEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!sampleEvent) {
            error = "CreateEvent failed";
            return false;
        }

        hr = audioClient->SetEventHandle(sampleEvent);
        if (FAILED(hr)) {
            error = "SetEventHandle failed: " + hresultText(hr);
            return false;
        }

        hr = audioClient->Start();
        if (FAILED(hr)) {
            error = "IAudioClient::Start failed: " + hresultText(hr);
            return false;
        }

        return true;
    }

    void captureLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        diag::log("wasapi capture thread started");
        startTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();

        while (running.load()) {
            const DWORD wait = WaitForSingleObject(sampleEvent, 100);
            if (!running.load()) {
                break;
            }
            if (wait != WAIT_OBJECT_0) {
                continue;
            }

            UINT32 packetFrames = 0;
            HRESULT nextHr = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(nextHr)) {
                diag::log("wasapi GetNextPacketSize failed: %s", hresultText(nextHr).c_str());
            }
            while (running.load() && SUCCEEDED(nextHr) && packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;

                const HRESULT hr = captureClient->GetBuffer(
                    &data, &framesAvailable, &flags, &devicePosition, &qpcPosition);
                if (FAILED(hr)) {
                    diag::log("wasapi GetBuffer failed: %s", hresultText(hr).c_str());
                    break;
                }

                CapturedAudioFrame frame;
                frame.sampleRate = kSampleRate;
                frame.channels = kChannels;
                frame.frames = static_cast<int>(framesAvailable);
                frame.timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();
                frame.samples.resize(static_cast<size_t>(framesAvailable) * kChannels);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(frame.samples.begin(), frame.samples.end(), 0);
                } else if (data) {
                    std::memcpy(frame.samples.data(), data,
                                static_cast<size_t>(framesAvailable) * kBlockAlign);
                }

                int peak = 0;
                for (int16_t sample : frame.samples) {
                    const int value = sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
                    if (value > peak) {
                        peak = value;
                    }
                }
                frame.peak = peak;

                captureClient->ReleaseBuffer(framesAvailable);

                if (running.load() && callback) {
                    callback(frame);
                }

                nextHr = captureClient->GetNextPacketSize(&packetFrames);
            }
        }

        if (audioClient) {
            audioClient->Stop();
        }
        diag::log("wasapi capture thread stopped");
        CoUninitialize();
    }

    void stop() {
        running.store(false);
        if (sampleEvent) {
            SetEvent(sampleEvent);
        }
        if (thread.joinable()) {
            thread.join();
        }
        thread = {};
        captureClient = nullptr;
        audioClient = nullptr;
        if (sampleEvent) {
            CloseHandle(sampleEvent);
            sampleEvent = nullptr;
        }
        callback = nullptr;
    }
};

WasapiCapture::WasapiCapture() : impl_(std::make_unique<Impl>()) {}

WasapiCapture::~WasapiCapture() = default;

bool WasapiCapture::startDevice(const std::wstring& deviceId, Callback callback) {
    stop();
    impl_->error.clear();
    impl_->callback = std::move(callback);

    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(enumerator.put()));
    if (FAILED(hr)) {
        impl_->error = "CoCreateInstance(MMDeviceEnumerator) failed: " + hresultText(hr);
        return false;
    }

    winrt::com_ptr<IMMDevice> device;
    if (deviceId.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    } else {
        hr = enumerator->GetDevice(deviceId.c_str(), device.put());
    }
    if (FAILED(hr)) {
        impl_->error = "GetDevice failed: " + hresultText(hr);
        return false;
    }

    winrt::com_ptr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void());
    if (FAILED(hr)) {
        impl_->error = "IMMDevice::Activate failed: " + hresultText(hr);
        return false;
    }

    if (!impl_->initializeClient(std::move(client))) {
        return false;
    }

    impl_->running.store(true);
    impl_->thread = std::thread([impl = impl_.get()] { impl->captureLoop(); });
    return true;
}

bool WasapiCapture::startProcess(DWORD processId, bool includeProcessTree, Callback callback) {
    stop();
    impl_->error.clear();
    impl_->callback = std::move(callback);

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = processId;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        includeProcessTree ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                           : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT prop{};
    PropVariantInit(&prop);
    prop.vt = VT_BLOB;
    prop.blob.cbSize = sizeof(params);
    prop.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto handler = Microsoft::WRL::Make<ActivationHandler>();
    if (!handler) {
        impl_->error = "ActivationHandler allocation failed";
        return false;
    }

    winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &prop,
        handler.Get(),
        operation.put());
    if (FAILED(hr)) {
        impl_->error = "ActivateAudioInterfaceAsync failed: " + hresultText(hr);
        return false;
    }

    const DWORD wait = WaitForSingleObject(handler->event(), 5000);
    if (wait != WAIT_OBJECT_0) {
        impl_->error = "ActivateAudioInterfaceAsync timed out";
        return false;
    }
    if (FAILED(handler->result) || !handler->client) {
        impl_->error = "Process loopback activation failed: " + hresultText(handler->result);
        return false;
    }

    winrt::com_ptr<IAudioClient> client;
    client.attach(handler->client.detach());
    if (!impl_->initializeClient(std::move(client))) {
        return false;
    }

    impl_->running.store(true);
    impl_->thread = std::thread([impl = impl_.get()] { impl->captureLoop(); });
    return true;
}

void WasapiCapture::stop() {
    if (impl_) {
        impl_->stop();
    }
}

bool WasapiCapture::isRunning() const {
    return impl_ && impl_->running.load();
}

std::string WasapiCapture::lastError() const {
    return impl_ ? impl_->error : std::string();
}

} // namespace ts3stream::media
