#pragma once
#include "lan_address.h"
#include "audio_stream.h"
#include "video_packets.h"
#include <QMetaType>
#include <QObject>
#include <memory>
#include <optional>

namespace wwmm
{
struct VideoReceptionStats {
    qint64 mediaBytesPerSecond = 0;
    qint64 videoBitsPerSecond = 0;
    std::optional<double> jitterMs, lossPercent;
};

class VideoLink final : public QObject
{
    Q_OBJECT
public:
    explicit VideoLink(bool sender, QObject *parent = nullptr);
    ~VideoLink() override;
    quint16 bind(const LanAddress &link);
    bool connectPeer(const Endpoint &peer, const QByteArray &keys, const QByteArray &audioKey = {});
    void setQuality(VideoQuality quality, bool keepEstimate = false);
    void setAudio(std::shared_ptr<AudioStream> stream);
    void submit(const QByteArray &annexB, quint32 timestamp);
    void requestKeyframe();
    void sendPointer(const QByteArray &input);
signals:
    void pointerReceived(const QByteArray &input);
    void frameReady(const QByteArray &annexB, quint32 timestamp);
    void keyframeNeeded();
    void bitrateChanged(int bitsPerSecond);
    void receptionSample(qint64 nowMs, int loss256, int jitterMs, bool queued);
    void receptionStats(const wwmm::VideoReceptionStats &stats);
    void failed(const QString &reason);
    void transportLost(const QString &reason);

private:
  void pumpAudio();
  void receive();
  void tick();
  void onRtp(QByteArrayView data, int wireSize);
  void reportReception(quint16 seq, quint32 stamp);
  void sampleReception();
  void onFeedback(QByteArrayView data);
  void deliver();
  void sendFeedback(bool pli, quint16 seq = 0, quint16 mask = 0);
  struct Impl;
  std::unique_ptr<Impl> p_;
};
} // namespace wwmm

Q_DECLARE_METATYPE(wwmm::VideoReceptionStats)
