#pragma once
#include "native_desktop.h"
#include <VideoToolbox/VideoToolbox.h>

namespace wwmm
{
class MacVideoDecoder
{
public:
    using Output = std::function<void(CVPixelBufferRef, quint32)>;
    MacVideoDecoder(Output output, MediaError failure);
    ~MacVideoDecoder();
    void push(const QByteArray &annexB, quint32 timestamp);

private:
    void reset();
    bool format(const QByteArray &sps, const QByteArray &pps);
    Output output_;
    MediaError failure_;
    QByteArray sps_, pps_;
    VTDecompressionSessionRef decoder_ = nullptr;
    CMVideoFormatDescriptionRef format_ = nullptr;
};
} // namespace wwmm
