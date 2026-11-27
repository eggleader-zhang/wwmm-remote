#pragma once
#include <d3d11.h>
#include <memory>

namespace wwmm {
// Desktop Duplication can return frames with the pointer already drawn by the driver.
class CursorlessCapture {
  public:
    CursorlessCapture();
    ~CursorlessCapture();
    HRESULT start(ID3D11Device *device, HMONITOR monitor, SIZE &size);
    HRESULT copyNext(ID3D11DeviceContext *context, ID3D11Texture2D *target, DWORD timeout);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace wwmm
