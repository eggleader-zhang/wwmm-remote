#pragma once
#include "desktop_display.h"
#include "video_packets.h"
#include "video_viewport.h"
#include <QByteArray>
#include <QString>
#include <chrono>
#include <functional>
#include <memory>

namespace wwmm
{
inline qint64 videoClockUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
inline quint32 rtpTime(qint64 microseconds)
{
    return quint32(microseconds * 9 / 100);
}
using EncodedFrame = std::function<void(QByteArray, quint32, bool)>;
using MediaError = std::function<void(QString)>;
using PresentedFrame = std::function<void(quint32)>;
class DesktopCapture
{
public:
    virtual ~DesktopCapture() = default;
    virtual void refresh() = 0;

    virtual void inputActivity() {}
    virtual void setBitrate(int bitsPerSecond) = 0;
};
class DesktopPlayer
{
public:
    virtual ~DesktopPlayer() = default;
    virtual void push(const QByteArray &annexB, quint32 timestamp) = 0;
    virtual void setViewport(VideoViewport viewport) = 0;
};

std::unique_ptr<DesktopCapture> captureDesktop(EncodedFrame output, MediaError failure, quint64 display = 0,
                                               VideoQuality quality = VideoQuality::FullHD, bool showCursor = true,
                                               std::function<void()> cursorUnavailable = {});
std::unique_ptr<DesktopPlayer> playDesktop(quintptr nativeView, PresentedFrame presented, MediaError failure);
} // namespace wwmm
