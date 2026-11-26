#include "pointer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdint>
#include <vector>
namespace wwmm
{
DesktopPointer::DesktopPointer(ID3D11Device *device, ID3D11DeviceContext *context,
                               DXGI_MODE_ROTATION rotation)
    : device_(device), context_(context), rotation_(rotation)
{
}
HRESULT DesktopPointer::pipeline()
{
    constexpr char shader[] = R"(
cbuffer Args : register(b0){int2 origin;int2 crop;uint kind;uint3 pad;};
Texture2D<float4> bg:register(t0);Texture2D<uint4> pointerShape:register(t1);
float4 vs(uint id:SV_VertexID):SV_Position{
    float2 uv=float2((id<<1)&2,id&2);return float4(uv.x*2-1,1-uv.y*2,0,1);
}
float4 ps(float4 p:SV_Position):SV_Target{
    int2 loc=int2(p.xy)-origin;
    float3 background=bg.Load(int3(loc,0)).rgb;
    uint4 cursor=pointerShape.Load(int3(loc+crop,0));
    uint3 old=(uint3)round(background*255.0);
    if(kind==1)return float4(float3((old & cursor.r) ^ cursor.g)/255.0,1);
    if(kind==4)return float4(float3(cursor.a ? (old ^ cursor.rgb) : cursor.rgb)/255.0,1);
    return float4(lerp(background,float3(cursor.rgb)/255.0,float(cursor.a)/255.0),1);
})";
    Ptr<ID3DBlob> shaderVS, shaderPS;
    auto hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "vs", "vs_4_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shaderVS, nullptr);
    if (FAILED(hr))
        return hr;
    hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "ps", "ps_4_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shaderPS, nullptr);
    if (FAILED(hr))
        return hr;
    hr = device_->CreateVertexShader(shaderVS->GetBufferPointer(), shaderVS->GetBufferSize(), nullptr,
                                     &vertex_);
    if (FAILED(hr))
        return hr;
    hr =
        device_->CreatePixelShader(shaderPS->GetBufferPointer(), shaderPS->GetBufferSize(), nullptr, &pixel_);
    if (FAILED(hr))
        return hr;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = 32;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->CreateBuffer(&d, nullptr, &uniforms_);
    if (FAILED(hr))
        return hr;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    return device_->CreateRasterizerState(&rd, &raster_);
}
HRESULT DesktopPointer::update(IDXGIOutputDuplication *dup, const DXGI_OUTDUPL_FRAME_INFO &info)
{
    if (info.LastMouseUpdateTime.QuadPart) {
        position_ = info.PointerPosition.Position;
        visible_ = info.PointerPosition.Visible;
    }
    if (!info.PointerShapeBufferSize)
        return S_OK;
    if (info.PointerShapeBufferSize > 4 * 1024 * 1024)
        return E_INVALIDARG;
    std::vector<BYTE> raw(info.PointerShapeBufferSize);
    UINT actual = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO s{};
    auto hr = dup->GetFramePointerShape(UINT(raw.size()), raw.data(), &actual, &s);
    if (FAILED(hr))
        return hr;
    const bool mono = s.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    originalW_ = s.Width;
    originalH_ = mono ? s.Height / 2 : s.Height;
    kind_ = s.Type;
    if (!originalW_ || !originalH_ || originalW_ > 1024 || originalH_ > 1024 || actual > raw.size() ||
        uint64_t(s.Pitch) * s.Height > actual || s.Pitch < (mono ? (originalW_ + 7) / 8 : originalW_ * 4))
        return E_INVALIDARG;
    const bool turn = rotation_ == DXGI_MODE_ROTATION_ROTATE90 || rotation_ == DXGI_MODE_ROTATION_ROTATE270;
    width_ = turn ? originalH_ : originalW_;
    height_ = turn ? originalW_ : originalH_;
    std::vector<BYTE> rgba(width_ * height_ * 4);
    for (UINT y = 0; y < height_; ++y)
        for (UINT x = 0; x < width_; ++x) {
            UINT px = x, py = y;
            if (rotation_ == DXGI_MODE_ROTATION_ROTATE90) {
                px = originalW_ - 1 - y;
                py = x;
            } else if (rotation_ == DXGI_MODE_ROTATION_ROTATE180) {
                px = originalW_ - 1 - x;
                py = originalH_ - 1 - y;
            } else if (rotation_ == DXGI_MODE_ROTATION_ROTATE270) {
                px = y;
                py = originalH_ - 1 - x;
            }
            auto *out = rgba.data() + 4 * (x + y * width_);
            if (mono) {
                const auto offset = py * s.Pitch + px / 8, mask = 0x80u >> (px % 8);
                out[0] = (raw[offset] & mask) ? 255 : 0;
                out[1] = (raw[offset + s.Pitch * originalH_] & mask) ? 255 : 0;
            } else {
                auto *in = raw.data() + py * s.Pitch + 4 * px;
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
                out[3] = in[3];
            }
        }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA contents{rgba.data(), width_ * 4, 0};
    Ptr<ID3D11Texture2D> shape;
    hr = device_->CreateTexture2D(&desc, &contents, &shape);
    if (FAILED(hr))
        return hr;
    shape_.Reset();
    hr = device_->CreateShaderResourceView(shape.Get(), nullptr, &shape_);
    if (FAILED(hr))
        return hr;
    background_.Reset();
    previous_.Reset();
    return vertex_ ? S_OK : pipeline();
}
HRESULT DesktopPointer::draw(ID3D11Texture2D *desktop)
{
    drawn_ = {};
    if (!visible_ || !shape_)
        return S_OK;
    D3D11_TEXTURE2D_DESC frame{};
    desktop->GetDesc(&frame);
    LONG x = position_.x, y = position_.y;
    if (rotation_ == DXGI_MODE_ROTATION_ROTATE90) {
        x = position_.y;
        y = LONG(frame.Height) - position_.x - LONG(originalW_);
    } else if (rotation_ == DXGI_MODE_ROTATION_ROTATE180) {
        x = LONG(frame.Width) - position_.x - LONG(originalW_);
        y = LONG(frame.Height) - position_.y - LONG(originalH_);
    } else if (rotation_ == DXGI_MODE_ROTATION_ROTATE270) {
        x = LONG(frame.Width) - position_.y - LONG(originalH_);
        y = position_.x;
    }
    RECT clip{std::max(0L, x), std::max(0L, y), std::min(LONG(frame.Width), x + LONG(width_)),
              std::min(LONG(frame.Height), y + LONG(height_))};
    if (clip.right <= clip.left || clip.bottom <= clip.top)
        return S_OK;
    if (!background_) {
        auto desc = frame;
        desc.Width = width_;
        desc.Height = height_;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        auto hr = device_->CreateTexture2D(&desc, nullptr, &background_);
        if (FAILED(hr))
            return hr;
        hr = device_->CreateShaderResourceView(background_.Get(), nullptr, &previous_);
        if (FAILED(hr))
            return hr;
    }
    Ptr<ID3D11RenderTargetView> target;
    auto hr = device_->CreateRenderTargetView(desktop, nullptr, &target);
    if (FAILED(hr))
        return hr;
    D3D11_BOX source{UINT(clip.left), UINT(clip.top), 0, UINT(clip.right), UINT(clip.bottom), 1};
    context_->CopySubresourceRegion(background_.Get(), 0, 0, 0, 0, desktop, 0, &source);
    int args[8] = {int(clip.left), int(clip.top), int(clip.left - x), int(clip.top - y), int(kind_)};
    context_->UpdateSubresource(uniforms_.Get(), 0, nullptr, args, 0, 0);
    D3D11_VIEWPORT vp{float(clip.left),
                      float(clip.top),
                      float(clip.right - clip.left),
                      float(clip.bottom - clip.top),
                      0,
                      1};
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, uniforms_.GetAddressOf());
    ID3D11ShaderResourceView *sources[] = {previous_.Get(), shape_.Get()};
    context_->PSSetShaderResources(0, 2, sources);
    context_->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    context_->Draw(3, 0);
    sources[0] = sources[1] = nullptr;
    context_->PSSetShaderResources(0, 2, sources);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    drawn_ = clip;
    return S_OK;
}
void DesktopPointer::restore(ID3D11Texture2D *desktop)
{
    if (drawn_.right <= drawn_.left)
        return;
    D3D11_BOX rect{0, 0, 0, UINT(drawn_.right - drawn_.left), UINT(drawn_.bottom - drawn_.top), 1};
    context_->CopySubresourceRegion(desktop, 0, UINT(drawn_.left), UINT(drawn_.top), 0, background_.Get(), 0,
                                    &rect);
    drawn_ = {};
}
} // namespace wwmm
