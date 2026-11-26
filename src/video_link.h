#pragma once
#include "lan_address.h"
#include "audio_stream.h"
#include "video_packets.h"
#include <QObject>
#include <memory>

namespace wwmm
{
class VideoLink final : public QObject
{
    Q_OBJECT
public:
    explicit VideoLink(bool sender, QObject *parent = nullptr);
    ~VideoLink() override;
    quint16 bind(const LanAddress &link);
    bool connectPeer(const Endpoint &peer, const QByteArray &keys, const QByteArray &audioKey = {});
    void setQuality(VideoQuality quality);
    void setAudio(std::shared_ptr<AudioStream> stream);
    void submit(const QByteArray &annexB, quint32 timestamp);
    void requestKeyframe();
    void sendPointer(const QByteArray &input);
signals:
    void pointerReceived(const QByteArray &input);
    void frameReady(const QByteArray &annexB, quint32 timestamp);
    void keyframeNeeded();
    void bitrateChanged(int bitsPerSecond);
    void failed(const QString &reason);

private:
  void pumpAudio();
  void receive();
  void tick();
  void onRtp(const QByteArray &data);
  void reportReception(quint16 seq, quint32 stamp);
  void onFeedback(const QByteArray &data);
  void deliver();
  void sendFeedback(bool pli, quint16 seq = 0, quint16 mask = 0);
  struct Impl;
  std::unique_ptr<Impl> p_;
};
} // namespace wwmm
