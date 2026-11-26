#include "desktop_session.h"
#include <QRandomGenerator>
#include <QtEndian>

namespace wwmm {
void DesktopSession::chooseQuality(VideoQuality quality)
{
    if (!rich_ || state_ != Viewing || !display_ || quality == quality_ || quality > VideoQuality::UHD)
        return;
    if (interactive_)
        sendInput(InputEvent{});
    if (!control_)
        return;
    displayReady_ = false;
    if (!control_->send(QByteArray("Q") + char(quality)))
        stop(tr("无法切换远端画质。"));
}

bool DesktopSession::reduceQuality()
{
    if (!rich_ || quality_ == VideoQuality::FullHD || (state_ != Sharing && state_ != Viewing))
        return false;
    const auto next = quality_ == VideoQuality::UHD ? VideoQuality::QHD : VideoQuality::FullHD;
    if (state_ == Sharing)
        applyQuality(next);
    else
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
        return stop(tr("声音控制连接已断开。"));
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
    if (data.size() == 5 && data[0] == 'U') {
        peerSound_ = qFromBigEndian<quint32>(data.constData() + 1);
        syncSound();
        return true;
    }
    if (data.size() != 2 || quint8(data[1]) > quint8(VideoQuality::UHD))
        return false;
    const auto quality = VideoQuality(quint8(data[1]));
    if (state_ == Sharing && data[0] == 'Q' && display_) {
        applyQuality(quality);
        return true;
    }
    if (state_ == Viewing && data[0] == 'q') {
        quality_ = quality;
        emit qualityChanged();
        return true;
    }
    return false;
}
} // namespace wwmm
