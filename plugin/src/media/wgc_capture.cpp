#include "wgc_capture.h"
#include "desktop_capture.h"

#include "../log.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using ::IGraphicsCaptureItemInterop;
using ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

namespace ts3stream::media {
namespace {

std::string hresultText(HRESULT hr) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

template <typename T>
T create_item_for(HWND window = nullptr, HMONITOR monitor = nullptr) {
    auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    HRESULT hr = E_FAIL;
    if (window) {
        hr = interop->CreateForWindow(window, guid_of<GraphicsCaptureItem>(), put_abi(item));
    } else if (monitor) {
        hr = interop->CreateForMonitor(monitor, guid_of<GraphicsCaptureItem>(), put_abi(item));
    }
    check_hresult(hr);
    return item;
}

bool supportsCaptureBorderControl() {
    try {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error&) {
            // TS3 may already have initialized this thread as STA.
        }
        return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired");
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

} // namespace

struct WgcCapture::Impl {
    std::atomic<bool> running{false};
    FrameCallback callback;
    std::string error;
    std::unique_ptr<DesktopCapture> desktopCapture;
    int targetFps = 30;

    winrt::com_ptr<ID3D11Device> d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    IDirect3DDevice direct3DDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool framePool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::com_ptr<ID3D11Texture2D> stagingTexture;
    int stagingWidth = 0;
    int stagingHeight = 0;
    int poolWidth = 0;
    int poolHeight = 0;
    bool apartmentInitialized = false;
    std::mutex frameMutex;
    int64_t lastAcceptedFrameUs = 0;
    std::atomic<int> frameIntervalUs{33333};
    winrt::event_token frameToken{};

    ~Impl() {
        stop();
        // Do not call winrt::uninit_apartment() here. TS3 owns the process-wide
        // COM apartment; uninitializing it from a plugin during shutdown makes
        // ucrtbase fail-fast when Qt/TS3 release the remaining COM objects.
    }

    bool ensureDevice() {
        if (d3dDevice) {
            return true;
        }

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            d3dDevice.put(),
            &featureLevel,
            d3dContext.put());
        if (FAILED(hr)) {
            error = "D3D11CreateDevice failed: " + hresultText(hr);
            return false;
        }

        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        hr = d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
        if (FAILED(hr)) {
            error = "QueryInterface(IDXGIDevice) failed: " + hresultText(hr);
            return false;
        }

        winrt::com_ptr<IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
        if (FAILED(hr)) {
            error = "CreateDirect3D11DeviceFromDXGIDevice failed: " + hresultText(hr);
            return false;
        }

        direct3DDevice = inspectable.as<IDirect3DDevice>();
        return true;
    }

    bool ensureStaging(int width, int height) {
        if (stagingTexture && stagingWidth == width && stagingHeight == height) {
            return true;
        }

        stagingTexture = nullptr;
        stagingWidth = width;
        stagingHeight = height;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, stagingTexture.put());
        if (FAILED(hr)) {
            error = "CreateTexture2D(staging) failed: " + hresultText(hr);
            return false;
        }
        return true;
    }

    void onFrame(const Direct3D11CaptureFramePool& sender, const winrt::Windows::Foundation::IInspectable&) {
        if (!running.load()) {
            return;
        }

        static std::atomic<int> callbackCounter{0};
        static std::atomic<int> deliveredCounter{0};
        const int callbackIndex = callbackCounter.fetch_add(1);
        if ((callbackIndex % 600) == 0) {
            diag::log("wgc callbacks=%d delivered=%d", callbackIndex,
                      deliveredCounter.load());
        }

        try {
        // Serialize acquisition as well as copying: Recreate must not race a
        // second callback holding an old frame or shutdown closing the pool.
        std::lock_guard<std::mutex> frameLock(frameMutex);
        if (!running.load()) return;
        auto frame = sender.TryGetNextFrame();
        if (!frame) {
            return;
        }
        deliveredCounter.fetch_add(1);

        const int64_t frameTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                        frame.SystemRelativeTime())
                                        .count();

        // WGC may invoke FrameArrived on multiple pool threads. Serialize the
        // expensive texture copy and callback so only one large buffer is
        // allocated at a time. Compare against the last *accepted* frame, not
        // the previous callback, otherwise every 60 Hz frame is rejected by a
        // 30 fps limit.
        const int intervalUs = frameIntervalUs.load();
        // Timestamps from a 60 Hz compositor quantize to ~16666 us. A strict
        // 33333 us threshold would drop every second frame at 30 FPS, so allow
        // a small jitter tolerance.
        const int thresholdUs = intervalUs > 0 ? std::max(0, intervalUs - 1500) : 0;
        if (thresholdUs > 0 && lastAcceptedFrameUs != 0 && frameTimeUs > lastAcceptedFrameUs &&
            frameTimeUs - lastAcceptedFrameUs < thresholdUs) {
            return;
        }
        lastAcceptedFrameUs = frameTimeUs;

        const auto contentSize = frame.ContentSize();
        const int width = static_cast<int>(contentSize.Width);
        const int height = static_cast<int>(contentSize.Height);
        if (width <= 0 || height <= 0) {
            return;
        }
        if (width != poolWidth || height != poolHeight) {
            // ContentSize changes immediately, but this frame still uses the
            // old pool's texture. Discard it and acquire a complete new frame.
            frame.Close();
            frame = nullptr;
            sender.Recreate(direct3DDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, contentSize);
            diag::log("wgc frame pool resized: %dx%d -> %dx%d", poolWidth, poolHeight, width, height);
            poolWidth = width;
            poolHeight = height;
            return;
        }

        const auto surface = frame.Surface();
        winrt::com_ptr<IDirect3DDxgiInterfaceAccess> access;
        HRESULT hr = winrt::get_unknown(surface)->QueryInterface(IID_PPV_ARGS(access.put()));
        if (FAILED(hr)) {
            error = "QueryInterface(IDirect3DDxgiInterfaceAccess) failed: " + hresultText(hr);
            return;
        }

        winrt::com_ptr<ID3D11Texture2D> texture;
        hr = access->GetInterface(IID_PPV_ARGS(texture.put()));
        if (FAILED(hr)) {
            error = "GetInterface(ID3D11Texture2D) failed: " + hresultText(hr);
            return;
        }

        D3D11_TEXTURE2D_DESC textureDesc{};
        texture->GetDesc(&textureDesc);
        if (textureDesc.Width < static_cast<UINT>(width) || textureDesc.Height < static_cast<UINT>(height)) {
            return;
        }
        if (!ensureStaging(width, height)) {
            return;
        }

        const D3D11_BOX region{0, 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), 1};
        d3dContext->CopySubresourceRegion(stagingTexture.get(), 0, 0, 0, 0, texture.get(), 0, &region);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = d3dContext->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            error = "Map(staging) failed: " + hresultText(hr);
            return;
        }

        CapturedVideoFrame out;
        out.width = width;
        out.height = height;
        out.stride = static_cast<int>(mapped.RowPitch);
        out.timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
        out.bgra.resize(static_cast<size_t>(mapped.RowPitch) * static_cast<size_t>(height));

        const auto* source = static_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < height; ++y) {
            std::memcpy(out.bgra.data() + static_cast<size_t>(y) * mapped.RowPitch,
                        source + static_cast<size_t>(y) * mapped.RowPitch,
                        mapped.RowPitch);
        }

        d3dContext->Unmap(stagingTexture.get(), 0);

        if (callback) {
            callback(std::move(out));
        }
        } catch (const winrt::hresult_error& e) {
            error = winrt::to_string(e.message());
            diag::log("wgc frame error: %s", error.c_str());
        } catch (...) {
            error = "wgc frame unknown error";
            diag::log("wgc frame unknown error");
        }
    }

    bool start(GraphicsCaptureItem captureItem, FrameCallback cb) {
        stop();

        callback = std::move(cb);
        error.clear();

        try {
            apartmentInitialized = true;
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error&) {
            // Already initialized on this thread; that's fine.
            apartmentInitialized = false;
        }

        if (!GraphicsCaptureSession::IsSupported()) {
            error = "Windows Graphics Capture is not supported";
            return false;
        }

        if (!ensureDevice()) {
            return false;
        }

        try {
            item = std::move(captureItem);
            const auto size = item.Size();
            poolWidth = size.Width;
            poolHeight = size.Height;
            lastAcceptedFrameUs = 0;
            framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                direct3DDevice,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                3,
                size);

            session = framePool.CreateCaptureSession(item);
            session.IsCursorCaptureEnabled(true);
            try {
                session.IsBorderRequired(false);
            } catch (...) {
                // Older Windows versions don't have this property.
            }

            frameToken = framePool.FrameArrived([this](const Direct3D11CaptureFramePool& sender,
                                                       const winrt::Windows::Foundation::IInspectable& args) {
                onFrame(sender, args);
            });

            running.store(true);
            session.StartCapture();
            return true;
        } catch (const winrt::hresult_error& e) {
            error = winrt::to_string(e.message());
            stop();
            return false;
        }
    }

    void stop() {
        if (desktopCapture) {
            desktopCapture->stop();
            desktopCapture.reset();
        }
        running.store(false);
        try {
            if (framePool && frameToken.value != 0) {
                framePool.FrameArrived(frameToken);
                frameToken = {};
            }
            // Wait for an in-flight FrameArrived callback: it serializes the
            // staging texture copy and the plugin callback under frameMutex.
            // Closing the pool/textures while that callback runs is a use-after-free.
            {
                std::lock_guard<std::mutex> frameLock(frameMutex);
            }
            if (session) {
                session.Close();
                session = nullptr;
            }
            if (framePool) {
                framePool.Close();
                framePool = nullptr;
            }
            item = nullptr;
        } catch (...) {
        }
        stagingTexture = nullptr;
        stagingWidth = 0;
        stagingHeight = 0;
        poolWidth = 0;
        poolHeight = 0;
        callback = nullptr;
    }
};

WgcCapture::WgcCapture() : impl_(std::make_unique<Impl>()) {}

WgcCapture::~WgcCapture() = default;

bool WgcCapture::startMonitor(HMONITOR monitor, FrameCallback callback) {
    if (!monitor) {
        impl_->error = "startMonitor: null monitor";
        return false;
    }
    // Released Windows 10 client builds lack IsBorderRequired. Silently
    // ignoring that failure leaves the yellow monitor outline on the streamer.
    // Select a genuinely borderless backend before creating any WGC session.
    if (!supportsCaptureBorderControl()) {
        impl_->stop();
        impl_->error.clear();
        impl_->desktopCapture = std::make_unique<DesktopCapture>();
        impl_->desktopCapture->setTargetFps(impl_->targetFps);
        diag::log("wgc border control unavailable; using DXGI for monitor capture");
        return impl_->desktopCapture->startMonitor(monitor, std::move(callback));
    }
    try {
        auto item = create_item_for<GraphicsCaptureItem>(nullptr, monitor);
        return impl_->start(std::move(item), std::move(callback));
    } catch (const winrt::hresult_error& e) {
        impl_->error = winrt::to_string(e.message());
        return false;
    }
}

bool WgcCapture::startWindow(HWND window, FrameCallback callback) {
    if (!window) {
        impl_->error = "startWindow: null window";
        return false;
    }
    try {
        auto item = create_item_for<GraphicsCaptureItem>(window, nullptr);
        return impl_->start(std::move(item), std::move(callback));
    } catch (const winrt::hresult_error& e) {
        impl_->error = winrt::to_string(e.message());
        return false;
    }
}

void WgcCapture::stop() {
    impl_->stop();
}

void WgcCapture::setTargetFps(int fps) {
    impl_->targetFps = fps;
    if (impl_->desktopCapture) impl_->desktopCapture->setTargetFps(fps);
    const int interval = fps > 0 ? 1000000 / fps : 0;
    impl_->frameIntervalUs.store(interval);
    std::lock_guard<std::mutex> lock(impl_->frameMutex);
    impl_->lastAcceptedFrameUs = 0;
}

bool WgcCapture::isRunning() const {
    if (impl_->desktopCapture) return impl_->desktopCapture->isRunning();
    return impl_->running.load();
}

std::string WgcCapture::lastError() const {
    if (impl_->desktopCapture) return impl_->desktopCapture->lastError();
    return impl_->error;
}

} // namespace ts3stream::media

