#pragma once
#include <QByteArray>
#include <QList>
#include <QSize>

namespace wwmm
{
constexpr int videoFps = 60;
constexpr int videoBitrate = 20000000;
constexpr int maxAccessUnit = 1024 * 1024;
constexpr int maxFragments = 1024;
constexpr quint32 videoSsrc = 0x57564d33, viewerSsrc = 0x57565233;

QSize desktopSize(QSize source);
QList<QByteArrayView> h264Units(const QByteArray &annexB);
QSize h264PictureSize(const QByteArray &annexB);
bool validAccessUnit(const QByteArray &annexB, bool &keyframe);
QList<QByteArray> packetizeVideo(const QByteArray &annexB, bool keyframe, quint16 &sequence,
                                 quint32 timestamp);
QByteArray assembleVideo(const QList<QByteArray> &payloads);
} // namespace wwmm
