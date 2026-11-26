#include "audio_stream.h"
#include <QMutex>
#include <QRandomGenerator>
#include <algorithm>
#include <array>
#include <deque>
#include <opus.h>

namespace wwmm {
struct AudioStream::State {
    OpusEncoder *encoder = nullptr;
    OpusDecoder *decoder = nullptr;
    QMutex mutex;
    std::deque<SoundPacket> packets;
    std::array<float, frames * channels> pcm{};
    quint32 captureStamp = QRandomGenerator::global()->generate();
    std::optional<quint32> next;
    int offset = 0, misses = 0;

    ~State()
    {
        if (encoder)
            opus_encoder_destroy(encoder);
        if (decoder)
            opus_decoder_destroy(decoder);
    }
};

AudioStream::AudioStream(bool sender, quint64 generation)
    : s_(std::make_unique<State>()), generation_(generation)
{
    int error = 0;
    if (sender) {
        s_->encoder = opus_encoder_create(rate, channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
        if (s_->encoder) {
            opus_encoder_ctl(s_->encoder, OPUS_SET_BITRATE(96000));
            opus_encoder_ctl(s_->encoder, OPUS_SET_COMPLEXITY(5));
        }
    } else {
        s_->decoder = opus_decoder_create(rate, channels, &error);
        s_->offset = frames;
    }
}

AudioStream::~AudioStream() = default;

std::shared_ptr<AudioStream> AudioStream::create(bool sender, quint64 generation)
{
    auto stream = std::shared_ptr<AudioStream>(new AudioStream(sender, generation));
    return stream->s_->encoder || stream->s_->decoder ? stream : nullptr;
}

void AudioStream::capture(const float *left, const float *right, int stride, int count)
{
    if (!s_->encoder)
        return;
    while (count > 0) {
        const int part = std::min(count, frames - s_->offset);
        for (int i = 0; i < part; ++i) {
            s_->pcm[2 * (s_->offset + i)] = left[i * stride];
            s_->pcm[2 * (s_->offset + i) + 1] = right[i * stride];
        }
        count -= part;
        if (count > 0) {
            left += part * stride;
            right += part * stride;
        }
        s_->offset += part;
        if (s_->offset != frames)
            continue;
        s_->offset = 0;
        const auto stamp = s_->captureStamp;
        s_->captureStamp += frames;
        QByteArray data(maxPacket, Qt::Uninitialized);
        const int n = opus_encode_float(s_->encoder, s_->pcm.data(), frames,
                                        reinterpret_cast<unsigned char *>(data.data()), maxPacket);
        if (n <= 0)
            continue;
        data.resize(n);
        QMutexLocker guard(&s_->mutex);
        if (s_->packets.size() >= 3)
            s_->packets.pop_front();
        s_->packets.push_back({stamp, std::move(data)});
    }
}

std::optional<SoundPacket> AudioStream::take()
{
    QMutexLocker guard(&s_->mutex);
    if (s_->packets.empty())
        return {};
    auto front = std::move(s_->packets.front());
    s_->packets.pop_front();
    return front;
}

void AudioStream::receive(quint32 stamp, QByteArray data)
{
    if (!s_->decoder || data.isEmpty() || data.size() > maxPacket ||
        opus_packet_get_nb_samples(reinterpret_cast<const unsigned char *>(data.constData()),
                                   int(data.size()), rate) != frames)
        return;
    QMutexLocker guard(&s_->mutex);
    auto &q = s_->packets;
    const auto pos =
        std::find_if(q.begin(), q.end(), [stamp](const auto &p) { return qint32(stamp - p.stamp) <= 0; });
    if (pos != q.end() && pos->stamp == stamp)
        return;
    q.insert(pos, {stamp, std::move(data)});
    while (q.size() > 6)
        q.pop_front();
}

void AudioStream::nextBlock()
{
    QByteArray data;
    {
        QMutexLocker guard(&s_->mutex);
        auto &q = s_->packets;
        if (s_->next) {
            while (!q.empty() && qint32(q.front().stamp - *s_->next) < 0)
                q.pop_front();
            if (!q.empty() && qint32(q.back().stamp - *s_->next) >= 5 * frames) {
                while (q.size() > 2)
                    q.pop_front();
                s_->next = q.front().stamp;
            }
        }
        if (!s_->next && q.size() >= 2)
            s_->next = q.front().stamp;
        if (s_->next) {
            if (!q.empty() && q.front().stamp == *s_->next) {
                data = std::move(q.front().payload);
                q.pop_front();
            }
            *s_->next += frames;
        }
    }
    s_->offset = 0;
    if (!s_->next || (data.isEmpty() && ++s_->misses > 2)) {
        s_->next.reset();
        s_->misses = 0;
        s_->pcm.fill(0);
        return;
    }
    if (!data.isEmpty())
        s_->misses = 0;
    auto *bytes = data.isEmpty() ? nullptr : reinterpret_cast<const unsigned char *>(data.constData());
    if (opus_decode_float(s_->decoder, bytes, int(data.size()), s_->pcm.data(), frames, 0) != frames)
        s_->pcm.fill(0);
}

void AudioStream::render(float *out, int count)
{
    if (!s_->decoder) {
        std::fill_n(out, count * channels, 0.f);
        return;
    }
    while (count > 0) {
        if (s_->offset == frames)
            nextBlock();
        const int part = std::min(count, frames - s_->offset);
        std::copy_n(s_->pcm.data() + channels * s_->offset, channels * part, out);
        out += channels * part;
        count -= part;
        s_->offset += part;
    }
}
} // namespace wwmm
