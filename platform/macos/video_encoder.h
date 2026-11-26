#pragma once
#include "native_desktop.h"
#include <QSize>
#include <VideoToolbox/VideoToolbox.h>
#include <atomic>

namespace wwmm
{
class MacVideoEncoder
{
  public:
    MacVideoEncoder(EncodedFrame output, MediaError failure);
    ~MacVideoEncoder();
    bool open(QSize size, int bitrate = videoBitrate);
    void setBitrate(int rate);
    bool encode(CVPixelBufferRef image, qint64 timeUs, bool force);

  private:
    static void encoded(void *context, void *, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef);
    EncodedFrame output_;
    MediaError failure_;
    VTCompressionSessionRef encoder_ = nullptr;
    std::atomic_int pending_{0};
};
} // namespace wwmm
