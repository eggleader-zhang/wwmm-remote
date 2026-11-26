#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace wwmm
{
// DXGI can deliver the hardware pointer separately from the desktop surface.
class DesktopPointer
{
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    Ptr<ID3D11VertexShader> vertex_;
    Ptr<ID3D11PixelShader> pixel_;
    Ptr<ID3D11Buffer> uniforms_;
    Ptr<ID3D11RasterizerState> raster_;
    Ptr<ID3D11Texture2D> background_;
    Ptr<ID3D11ShaderResourceView> shape_, previous_;
    POINT position_{};
    DXGI_MODE_ROTATION rotation_;
    UINT width_ = 0, height_ = 0, originalW_ = 0, originalH_ = 0, kind_ = 0;
    bool visible_ = false;
    RECT drawn_{};
    HRESULT pipeline();

public:
    DesktopPointer(ID3D11Device *device, ID3D11DeviceContext *context, DXGI_MODE_ROTATION rotation);
    HRESULT update(IDXGIOutputDuplication *dup, const DXGI_OUTDUPL_FRAME_INFO &info);
    HRESULT draw(ID3D11Texture2D *desktop);
    void restore(ID3D11Texture2D *desktop);
};
} // namespace wwmm
