#pragma once
#include "audio_stream.h"
#include <QString>
#include <functional>

namespace wwmm {
class NativeAudio {
  public:
    virtual ~NativeAudio() = default;
};

std::unique_ptr<NativeAudio> openSystemAudio(bool capture, std::shared_ptr<AudioStream> stream,
                                             std::function<void(QString)> failed);
} // namespace wwmm
