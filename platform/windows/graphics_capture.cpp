#include "graphics_capture.h"
#if defined(_MSC_VER) || (__has_include(<winrt/Windows.Graphics.Capture.h>) && __has_include(<windows.graphics.capture.interop.h>))
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <atomic>
#include <utility>

namespace wwmm {
namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
struct CursorlessCapture::State {
    struct Wake {
        winrt::handle ready{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        std::atomic_bool closed{false};
    };
    std::shared_ptr<Wake> wake = std::make_shared<Wake>();
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 dimensions{};
    ~State()
    {
        try {
            if (session)
                session.Close();
            if (pool)
                pool.Close();
        } catch (const winrt::hresult_error &) {
            // A display may already have been removed.
        }
    }
};

HRESULT CursorlessCapture::start(ID3D11Device *device, HMONITOR monitor, SIZE &size)
{
    try {
        state_ = std::make_unique<State>();
        auto &s = *state_;
        if (!s.wake->ready)
            return HRESULT_FROM_WIN32(GetLastError());
        auto factory = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForMonitor(monitor, winrt::guid_of<capture::GraphicsCaptureItem>(),
                                                       winrt::put_abi(s.item)));
        s.dimensions = s.item.Size();
        if (s.dimensions.Width <= 0 || s.dimensions.Height <= 0)
            return DXGI_ERROR_ACCESS_LOST;
        size = {s.dimensions.Width, s.dimensions.Height};
        winrt::com_ptr<IDXGIDevice> dxgi;
        winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgi.put())));
        winrt::com_ptr<IInspectable> wrapped;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), wrapped.put()));
        s.pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            wrapped.as<directx::Direct3D11::IDirect3DDevice>(), directx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, s.dimensions);
        s.session = s.pool.CreateCaptureSession(s.item);
        auto cursorSettings = s.session.try_as<capture::IGraphicsCaptureSession2>();
        if (!cursorSettings)
            return E_NOINTERFACE;
        cursorSettings.IsCursorCaptureEnabled(false);
        s.pool.FrameArrived([wake = s.wake](auto const &, auto const &) { SetEvent(wake->ready.get()); });
        s.item.Closed([wake = s.wake](auto const &, auto const &) {
            wake->closed = true;
            SetEvent(wake->ready.get());
        });
        s.session.StartCapture();
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
}

HRESULT CursorlessCapture::copyNext(ID3D11DeviceContext *context, ID3D11Texture2D *target, DWORD timeout)
{
    try {
        auto &s = *state_;
        if (s.wake->closed)
            return DXGI_ERROR_ACCESS_LOST;
        auto frame = s.pool.TryGetNextFrame();
        if (!frame) {
            const auto wait = WaitForSingleObject(s.wake->ready.get(), timeout);
            if (wait == WAIT_FAILED)
                return HRESULT_FROM_WIN32(GetLastError());
            if (s.wake->closed)
                return DXGI_ERROR_ACCESS_LOST;
            if (wait == WAIT_TIMEOUT || !(frame = s.pool.TryGetNextFrame()))
                return DXGI_ERROR_WAIT_TIMEOUT;
        }
        if (auto latest = s.pool.TryGetNextFrame()) {
            frame.Close();
            frame = std::move(latest);
        }
        const auto size = frame.ContentSize();
        if (size.Width != s.dimensions.Width || size.Height != s.dimensions.Height)
            return DXGI_ERROR_ACCESS_LOST;
        auto surface = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(surface->GetInterface(IID_PPV_ARGS(texture.put())));
        context->CopyResource(target, texture.get());
        frame.Close();
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
}
#else
namespace wwmm {
struct CursorlessCapture::State {};
HRESULT CursorlessCapture::start(ID3D11Device *, HMONITOR, SIZE &)
{
    return E_NOTIMPL;
}
HRESULT CursorlessCapture::copyNext(ID3D11DeviceContext *, ID3D11Texture2D *, DWORD)
{
    return E_NOTIMPL;
}
#endif

CursorlessCapture::CursorlessCapture() = default;
CursorlessCapture::~CursorlessCapture() = default;
} // namespace wwmm
