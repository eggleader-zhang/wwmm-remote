#pragma once
#include <QByteArray>
#include <QList>
#include <QSize>

namespace wwmm
{
constexpr int videoFps = 60;
constexpr int videoBitrate = 20000000;
enum class VideoQuality : quint8 { FullHD, QHD, UHD };
constexpr int maxAccessUnit = 4 * 1024 * 1024;
constexpr int maxFragments = 4096;
constexpr quint32 videoSsrc = 0x57564d33, viewerSsrc = 0x57565233;

int qualityBitrate(VideoQuality quality);
QSize desktopSize(QSize source, VideoQuality quality = VideoQuality::FullHD);
QList<QByteArrayView> h264Units(const QByteArray &annexB);
QSize h264PictureSize(const QByteArray &annexB);
bool validAccessUnit(const QByteArray &annexB, bool &keyframe);
QList<QByteArray> packetizeVideo(const QByteArray &annexB, bool keyframe, quint16 &sequence,
                                 quint32 timestamp);
QByteArray assembleVideo(const QList<QByteArray> &payloads);
} // namespace wwmm
