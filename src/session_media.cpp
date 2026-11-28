#include "desktop_session.h"
#include <QRandomGenerator>
#include <QtEndian>

namespace wwmm {
void DesktopSession::appendMediaOptions(QByteArray &message) const
{
    if (cursorProtocol_) {
        message.append(char(frameRateProtocol_ ? 18 : rttProtocol_ ? 17 : 16));
        message.append(char(captureCursor_));
    } else if (rich_) {
        message.append(char(macKeyboardProtocol_ ? 15 : adaptive_ ? 14 : 13));
    }
}

void DesktopSession::probeRtt()
{
    if (state_ != Viewing || !rttProtocol_ || !control_)
        return;
    const auto token = ++rttSequence_;
    QByteArray message(5, 'k');
    qToBigEndian(token, message.data() + 1);
    pendingRttToken_ = token;
    rttClock_.start();
    if (!control_->send(message))
        interrupt(tr("桌面连接已断开。"));
}

void DesktopSession::setRemoteCursorVisible(bool visible)
{
    preferCursor_ = visible;
    if (state_ != Viewing || !canSetRemoteCursor() || captureCursor_ == visible)
        return;
    if (!control_->send(QByteArray("P") + char(visible)))
        return interrupt(tr("鼠标显示设置连接已断开。"));
    captureCursor_ = visible;
}

void DesktopSession::reportCursorUnavailable()
{
    if (state_ != Sharing || !canSetRemoteCursor() || captureCursor_)
        return;
    if (!control_->send("p"))
        return interrupt(tr("鼠标显示设置连接已断开。"));
    cursorUnavailable_ = captureCursor_ = true;
    emit captureCursorChanged();
}

void DesktopSession::chooseQuality(VideoQuality quality)
{
    if (!rich_ || state_ != Viewing || !display_ || quality == qualityLimit_ ||
        quality < VideoQuality::FullHD || quality > VideoQuality::UHD)
        return;
    if (interactive_ && !adaptive_)
        sendInput(InputEvent{});
    if (!control_)
        return;
    if (!adaptive_)
        displayReady_ = false;
    if (!control_->send(QByteArray("Q") + char(quality)))
        interrupt(tr("无法切换远端画质。"));
}

bool DesktopSession::reduceQuality()
{
    if (!rich_ || quality_ <= (adaptive_ ? VideoQuality::HD : VideoQuality::FullHD) ||
        (state_ != Sharing && state_ != Viewing))
        return false;
    const auto next = VideoQuality(int(quality_) - 1);
    if (state_ == Sharing) {
        hardwareLimit_ = next;
        if (!adaptive_)
            qualityLimit_ = next;
        resolution_ = {};
        applyQuality(next);
    } else if (adaptive_) {
        if (!control_->send(QByteArray("H") + char(quality_)))
            interrupt(tr("画质连接已断开。"));
    } else
        chooseQuality(next);
    emit notice(tr("当前硬件或连接无法维持所选画质，已请求较低分辨率。"));
    return true;
}

void DesktopSession::chooseFrameRate(int fps)
{
    if (state_ == Viewing && frameRateProtocol_ && validFrameRate(fps) &&
        !control_->send(QByteArray("G") + char(fps)))
        interrupt(tr("帧率设置连接已断开。"));
}

void DesktopSession::applyFrameRate(int fps, bool restartCapture)
{
    const int active = qMin(fps, hardwareFrameRateLimit_);
    const bool changed = active != frameRate_;
    frameRate_ = active;
    if (!control_->send(QByteArray("g") + char(active) + char(frameRateLimit_)))
        return interrupt(tr("帧率设置连接已断开。"));
    if (changed)
        updateVideoBitrate();
    emit frameRateChanged();
    emit connectionStatsChanged();
    if (changed && restartCapture && display_)
        startDisplay(display_, adaptive_ && displayReady_);
}

bool DesktopSession::reduceFrameRate(const QString &reason)
{
    if (!frameRateProtocol_ || frameRate_ <= videoFps || (state_ != Sharing && state_ != Viewing))
        return false;
    if (state_ == Sharing) {
        hardwareFrameRateLimit_ = videoFps;
        applyFrameRate(videoFps);
        if (state_ == Sharing)
            emit notice(reason.isEmpty() ? tr("当前硬件或显示器不支持 120 fps，已自动回退至 60 fps。") : reason);
    } else if (!frameRateFallbackPending_) {
        frameRateFallbackPending_ = true;
        stateClock_.restart();
        if (!control_->send(QByteArray("h") + char(frameRate_)))
            interrupt(tr("帧率设置连接已断开。"));
        else
            emit notice(reason.isEmpty() ? tr("当前硬件无法显示 120 fps，已请求回退至 60 fps。") : reason);
    }
    return true;
}

void DesktopSession::reportFrameRateFallback(int activeFps, const QString &reason)
{
    if (state_ != Sharing || !frameRateProtocol_ || !validFrameRate(activeFps) || activeFps >= frameRate_)
        return;
    const int previous = frameRate_;
    hardwareFrameRateLimit_ = activeFps;
    applyFrameRate(activeFps, false);
    if (state_ == Sharing)
        emit notice(reason.isEmpty() ? tr("当前硬件或显示器不支持 %1 fps，已自动回退至 %2 fps。")
                                          .arg(previous).arg(activeFps) : reason);
}

void DesktopSession::enableSound(bool enable)
{
    if (!rich_ || (state_ != Viewing && state_ != Sharing) || enable == localSound())
        return;
    localSound_ = enable ? (QRandomGenerator::system()->generate() | 1u) : 0;
    QByteArray msg(5, 'U');
    qToBigEndian(localSound_, msg.data() + 1);
    if (!control_->send(msg))
        return interrupt(tr("声音控制连接已断开。"));
    syncSound();
}

void DesktopSession::syncSound()
{
    if (localSound_ && peerSound_) {
        const quint64 host = host_ ? localSound_ : peerSound_;
        const quint64 viewer = host_ ? peerSound_ : localSound_;
        const auto generation = (host << 32) | viewer;
        if (!sound_ || sound_->generation() != generation)
            sound_ = AudioStream::create(host_, generation);
        if (!sound_) {
            enableSound(false);
            emit notice(tr("无法打开低延迟声音编解码器。"));
            return;
        }
    } else
        sound_.reset();
    attachSound();
    emit soundChanged();
}

bool DesktopSession::mediaMessage(const QByteArray &data)
{
    if (frameRateProtocol_ && data.size() >= 2) {
        const int fps = quint8(data[1]);
        if (state_ == Sharing && data.size() == 2 && validFrameRate(fps)) {
            if (data[0] == 'G') {
                hardwareFrameRateLimit_ = 120;
                frameRateLimit_ = fps;
                applyFrameRate(fps);
                return true;
            }
            if (data[0] == 'h') {
                if (fps == frameRate_)
                    reduceFrameRate();
                return true;
            }
        }
        if (state_ == Viewing && data.size() == 3 && data[0] == 'g') {
            const int limit = quint8(data[2]);
            if (!validFrameRate(fps) || !validFrameRate(limit) || fps > limit)
                return false;
            const bool fallback = fps < limit && !frameRateFallbackPending_ &&
                                  (fps != frameRate_ || limit != frameRateLimit_);
            if (fps != frameRate_ && !received_)
                stateClock_.restart();
            frameRate_ = fps;
            frameRateLimit_ = limit;
            if (fps <= videoFps)
                frameRateFallbackPending_ = false;
            emit frameRateChanged();
            emit connectionStatsChanged();
            if (fallback)
                emit notice(tr("已自动回退至 %1 fps。").arg(fps));
            return true;
        }
    }
    if (rttProtocol_ && data.size() == 5) {
        if (state_ == Sharing && data[0] == 'k') {
            auto reply = data;
            reply[0] = 'r';
            if (!control_->send(reply))
                interrupt(tr("桌面连接已断开。"));
            return true;
        }
        if (state_ == Viewing && data[0] == 'r') {
            if (pendingRttToken_ == qFromBigEndian<quint32>(data.constData() + 1)) {
                stats_.rttMs = double(rttClock_.nsecsElapsed()) / 1000000.0;
                pendingRttToken_.reset();
                emit connectionStatsChanged();
            }
            return true;
        }
    }
    if (cursorProtocol_ && state_ == Sharing && data.size() == 2 && data[0] == 'P' && quint8(data[1]) <= 1) {
        const bool visible = data[1] != 0;
        if (!cursorUnavailable_ && captureCursor_ != visible) {
            captureCursor_ = visible;
            emit captureCursorChanged();
        }
        return true;
    }
    if (cursorProtocol_ && state_ == Viewing && data == "p") {
        cursorUnavailable_ = captureCursor_ = true;
        emit remoteCursorUnavailable();
        return true;
    }
    if (data.size() == 5 && data[0] == 'U') {
        peerSound_ = qFromBigEndian<quint32>(data.constData() + 1);
        syncSound();
        return true;
    }
    if (data.size() < 2 || data.size() > 3)
        return false;
    const auto quality = VideoQuality(qint8(data[1]));
    if (quality < (adaptive_ ? VideoQuality::HD : VideoQuality::FullHD) || quality > VideoQuality::UHD)
        return false;
    if (state_ == Sharing && display_ && data.size() == 2 && data[0] == 'H' && adaptive_) {
        if (quality == quality_ && !reduceQuality())
            stop(tr("远端硬件无法显示最低画质。"));
        return true;
    }
    if (state_ == Sharing && data.size() == 2 && data[0] == 'Q' && display_ && quality >= VideoQuality::FullHD) {
        qualityLimit_ = quality;
        hardwareLimit_ = VideoQuality::UHD;
        resolution_ = {};
        applyQuality(quality);
        return true;
    }
    if (state_ == Viewing && data[0] == 'q' && data.size() == (adaptive_ ? 3 : 2)) {
        auto limit = adaptive_ ? VideoQuality(qint8(data[2])) : quality;
        if (limit < VideoQuality::FullHD || limit > VideoQuality::UHD || quality > limit)
            return false;
        qualityLimit_ = limit;
        quality_ = quality;
        emit qualityChanged();
        emit connectionStatsChanged();
        return true;
    }
    return false;
}
} // namespace wwmm
