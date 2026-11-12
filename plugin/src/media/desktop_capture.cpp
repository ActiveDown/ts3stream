#include "desktop_capture.h"
#include "desktop_frame.h"
#include "../log.h"

#include <d3d11.h>
#include <winrt/base.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace ts3stream::media {
namespace {

// Release on every path, including exceptions during allocation or callbacks.
struct AcquiredFrame {
    IDXGIOutputDuplication* duplication;
    ~AcquiredFrame() { duplication->ReleaseFrame(); }
};

struct MappedTexture {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    ~MappedTexture() { context->Unmap(texture, 0); }
};

} // namespace

struct DesktopCapture::Impl {
    HMONITOR monitor = nullptr;
    FrameCallback callback;
    std::atomic<bool> running{false};
    std::atomic<int> intervalUs{33333};
    std::thread thread;
    std::mutex waitMutex;
    std::condition_variable wake;
    mutable std::mutex errorMutex;
    std::string error;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<IDXGIOutputDuplication> duplication;
    winrt::com_ptr<ID3D11Texture2D> staging;
    DXGI_OUTDUPL_DESC desc{};
    DXGI_OUTDUPL_POINTER_POSITION pointer{};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerShape{};
    std::vector<uint8_t> pointerPixels;

    ~Impl() { stop(); }

    void setError(const std::string& message) {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (error != message && !message.empty()) diag::log("dxgi capture: %s", message.c_str());
        error = message;
    }

    void resetDevice() {
        staging = nullptr;
        duplication = nullptr;
        context = nullptr;
        device = nullptr;
        pointer = {};
        pointerShape = {};
        pointerPixels.clear();
    }

    bool openMonitor() {
        resetDevice();
        try {
            winrt::com_ptr<IDXGIFactory1> factory;
            winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())));
            for (UINT adapterIndex = 0; ; ++adapterIndex) {
                winrt::com_ptr<IDXGIAdapter1> adapter;
                const HRESULT adapterHr = factory->EnumAdapters1(adapterIndex, adapter.put());
                if (adapterHr == DXGI_ERROR_NOT_FOUND) break;
                winrt::check_hresult(adapterHr);
                for (UINT outputIndex = 0; ; ++outputIndex) {
                    winrt::com_ptr<IDXGIOutput> output;
                    const HRESULT outputHr = adapter->EnumOutputs(outputIndex, output.put());
                    if (outputHr == DXGI_ERROR_NOT_FOUND) break;
                    winrt::check_hresult(outputHr);
                    DXGI_OUTPUT_DESC outputDesc{};
                    winrt::check_hresult(output->GetDesc(&outputDesc));
                    if (outputDesc.Monitor != monitor || !outputDesc.AttachedToDesktop) continue;

                    // DuplicateOutput requires a device on the monitor's adapter,
                    // not necessarily the default GPU (hybrid/multi-GPU machines).
                    winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                        device.put(), nullptr, context.put()));
                    winrt::check_hresult(output.as<IDXGIOutput1>()->DuplicateOutput(device.get(), duplication.put()));
                    duplication->GetDesc(&desc);
                    diag::log("dxgi monitor capture: %ux%u rotation=%d (no WGC border)",
                              desc.ModeDesc.Width, desc.ModeDesc.Height, static_cast<int>(desc.Rotation));
                    setError({});
                    return true;
                }
            }
            setError("Selected monitor is no longer attached");
        } catch (const winrt::hresult_error& e) {
            setError("Cannot duplicate selected monitor: " + winrt::to_string(e.message()));
        }
        return false;
    }

    void readPointer(const DXGI_OUTDUPL_FRAME_INFO& info) {
        if (info.LastMouseUpdateTime.QuadPart != 0) pointer = info.PointerPosition;
        if (info.PointerShapeBufferSize == 0) return;
        pointerPixels.resize(info.PointerShapeBufferSize);
        UINT needed = 0;
        winrt::check_hresult(duplication->GetFramePointerShape(static_cast<UINT>(pointerPixels.size()),
            pointerPixels.data(), &needed, &pointerShape));
    }

    CapturedVideoFrame readFrame(IDXGIResource* resource) {
        winrt::com_ptr<IDXGIResource> source;
        source.copy_from(resource);
        auto texture = source.as<ID3D11Texture2D>();
        D3D11_TEXTURE2D_DESC textureDesc{};
        texture->GetDesc(&textureDesc);
        if (staging) {
            D3D11_TEXTURE2D_DESC old{};
            staging->GetDesc(&old);
            if (old.Width != textureDesc.Width || old.Height != textureDesc.Height) staging = nullptr;
        }
        if (!staging) {
            textureDesc.Usage = D3D11_USAGE_STAGING;
            textureDesc.BindFlags = 0;
            textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            textureDesc.MiscFlags = 0;
            winrt::check_hresult(device->CreateTexture2D(&textureDesc, nullptr, staging.put()));
        }
        context->CopyResource(staging.get(), texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        CapturedVideoFrame frame;
        {
            MappedTexture unmap{context.get(), staging.get()};
            frame = detail::copyDesktopFrame(static_cast<const uint8_t*>(mapped.pData),
                static_cast<int>(textureDesc.Width), static_cast<int>(textureDesc.Height),
                static_cast<int>(mapped.RowPitch), desc.Rotation);
        }
        frame.timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (pointer.Visible) detail::drawDesktopPointer(frame, pointer.Position, pointerShape, pointerPixels);
        return frame;
    }

    void run() {
        auto nextFrame = std::chrono::steady_clock::now();
        while (running.load()) {
            {
                std::unique_lock<std::mutex> lock(waitMutex);
                wake.wait_until(lock, nextFrame, [this] { return !running.load(); });
            }
            if (!running.load()) break;
            if (!duplication && !openMonitor()) {
                nextFrame = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                continue;
            }
            DXGI_OUTDUPL_FRAME_INFO info{};
            winrt::com_ptr<IDXGIResource> resource;
            const HRESULT hr = duplication->AcquireNextFrame(50, &info, resource.put());
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) {
                // Lock/unlock, display-mode changes and GPU resets invalidate
                // duplication. Reopen the same monitor, never another display or WGC.
                char message[96]{};
                std::snprintf(message, sizeof(message), "AcquireNextFrame failed: HRESULT 0x%08lX",
                              static_cast<unsigned long>(hr));
                setError(message);
                resetDevice();
                nextFrame = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                continue;
            }
            const auto acquiredAt = std::chrono::steady_clock::now();
            try {
                CapturedVideoFrame frame;
                {
                    AcquiredFrame release{duplication.get()};
                    readPointer(info);
                    frame = readFrame(resource.get());
                }
                // Do not hold a DXGI frame while the consumer runs.
                if (running.load() && callback) callback(std::move(frame));
            } catch (const winrt::hresult_error& e) {
                setError(winrt::to_string(e.message()));
                resetDevice();
            } catch (const std::exception& e) {
                setError(e.what());
                running.store(false);
            } catch (...) {
                setError("Unknown desktop capture error");
                running.store(false);
            }
            nextFrame = acquiredAt + std::chrono::microseconds(intervalUs.load());
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(waitMutex);
            running.store(false);
        }
        wake.notify_all();
        if (thread.joinable()) thread.join();
        resetDevice();
        callback = nullptr;
    }
};

DesktopCapture::DesktopCapture() : impl_(std::make_unique<Impl>()) {}
DesktopCapture::~DesktopCapture() = default;

bool DesktopCapture::startMonitor(HMONITOR monitor, FrameCallback callback) {
    stop();
    impl_->setError({});
    if (!monitor) {
        impl_->setError("startMonitor: null monitor");
        return false;
    }
    impl_->monitor = monitor;
    impl_->callback = std::move(callback);
    if (!impl_->openMonitor()) {
        impl_->resetDevice();
        impl_->callback = nullptr;
        return false;
    }
    impl_->running.store(true);
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    return true;
}

void DesktopCapture::setTargetFps(int fps) { impl_->intervalUs.store(fps > 0 ? 1000000 / fps : 0); }
void DesktopCapture::stop() { impl_->stop(); }
bool DesktopCapture::isRunning() const { return impl_->running.load(); }
std::string DesktopCapture::lastError() const {
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    return impl_->error;
}

} // namespace ts3stream::media
