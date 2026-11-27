#include "desktop_session.h"
#include <QRandomGenerator>
#include <QtEndian>

namespace wwmm {
void DesktopSession::appendMediaOptions(QByteArray &message) const
{
    if (cursorProtocol_) {
        message.append(char(16));
        message.append(char(captureCursor_));
    } else if (rich_) {
        message.append(char(macKeyboardProtocol_ ? 15 : adaptive_ ? 14 : 13));
    }
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
        return true;
    }
    return false;
}
} // namespace wwmm
