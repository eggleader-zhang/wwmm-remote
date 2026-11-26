#pragma once
#include <QByteArray>
#include <memory>
#include <optional>

namespace wwmm {
struct SoundPacket {
    quint32 stamp = 0;
    QByteArray payload;
};

// Native audio callbacks and the media network thread communicate through bounded packet queues.
class AudioStream {
  public:
    static constexpr int rate = 48000, channels = 2, frames = 480, maxPacket = 1000;
    static std::shared_ptr<AudioStream> create(bool sender, quint64 generation);
    ~AudioStream();

    quint64 generation() const { return generation_; }

    void capture(const float *left, const float *right, int stride, int count);
    std::optional<SoundPacket> take();
    void receive(quint32 stamp, QByteArray payload);
    void render(float *interleaved, int count);

  private:
    struct State;
    AudioStream(bool sender, quint64 generation);
    void nextBlock();
    std::unique_ptr<State> s_;
    quint64 generation_;
};
} // namespace wwmm
