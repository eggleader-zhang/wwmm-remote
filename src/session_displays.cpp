#include "desktop_session.h"
#include "native_desktop.h"
#include <QtEndian>
#include <algorithm>

namespace wwmm {
namespace {
constexpr int displayLimit = 32;

auto findDisplay(const QList<DesktopDisplay> &list, quint64 id)
{
    return std::find_if(list.cbegin(), list.cend(), [id](const DesktopDisplay &d) { return d.id == id; });
}

QByteArray selection(char op, quint64 id)
{
    QByteArray msg(9, op);
    qToBigEndian(id, msg.data() + 1);
    return msg;
}
} // namespace

void DesktopSession::updateHostDisplays(QList<DesktopDisplay> available, bool restart)
{
    if (state_ != Sharing)
        return;
    if (available.isEmpty())
        return stop(tr("共享端已没有可用的显示器。"));
    if (available.size() > displayLimit)
        available.resize(displayLimit);
    const auto old = findDisplay(displays_, display_);
    const auto selected = findDisplay(available, display_);
    const bool current = selected != available.cend() &&
                         (old == displays_.cend() || old->persistentId == selected->persistentId);
    const bool resized = current && old != displays_.cend() && old->pixels != selected->pixels;
    const auto primary =
        std::find_if(available.cbegin(), available.cend(), [](const auto &d) { return d.primary; });
    auto first = primary == available.cend() ? available.cbegin() : primary;
    if (!current) {
        rememberOnPresent_.clear();
        if (!display_ && extended_) {
            const auto saved = store_.preferredDisplay(selected_);
            if (saved.isEmpty())
                rememberOnPresent_ = first->persistentId;
            else {
                auto matches = [&saved](const DesktopDisplay &d) { return d.persistentId == saved; };
                if (std::count_if(available.cbegin(), available.cend(), matches) == 1)
                    first = std::find_if(available.cbegin(), available.cend(), matches);
            }
        }
    }
    const auto id = current ? display_ : first->id;
    if (available != displays_) {
        displays_ = std::move(available);
        if (extended_) {
            if (!control_->send("L"))
                return interrupt(tr("显示器列表连接已断开。"));
            for (const auto &d : displays_) {
                QByteArray msg = selection('d', d.id);
                msg.resize(14);
                qToBigEndian(quint16(qBound(1, d.pixels.width(), 65535)), msg.data() + 9);
                qToBigEndian(quint16(qBound(1, d.pixels.height(), 65535)), msg.data() + 11);
                msg[13] = d.primary ? 1 : 0;
                msg += d.name.left(55).toUtf8();
                if (!control_->send(msg))
                    return interrupt(tr("显示器列表连接已断开。"));
            }
            if (!control_->send("l"))
                return interrupt(tr("显示器列表连接已断开。"));
        }
        emit displaysChanged();
    }
    if (!current || resized || restart) {
        if (!extended_ && display_)
            return stop(tr("显示器已改变，请重新连接桌面。"));
        startDisplay(id);
    }
}

void DesktopSession::startDisplay(quint64 id, bool formatOnly)
{
    display_ = id;
    displayReady_ = formatOnly || !extended_;
    if (extended_) {
        // Both capture backends timestamp frames on this same monotonic 90 kHz clock.
        boundary_ = rtpTime(videoClockUs());
        auto msg = selection(formatOnly ? 't' : 'T', id);
        msg.resize(13);
        qToBigEndian(boundary_, msg.data() + 9);
        if (!control_->send(msg))
            return interrupt(tr("显示器连接已断开。"));
    }
    lastSent_.reset();
    stillFrame_.stop();
    emit displaySwitched(formatOnly);
}

void DesktopSession::chooseDisplay(quint64 id)
{
    if (state_ != Viewing || !extended_ || !displayReady_ || id == display_ ||
        findDisplay(displays_, id) == displays_.cend())
        return;
    if (interactive_)
        sendInput(InputEvent{});
    if (!control_)
        return;
    displayReady_ = false;
    if (!control_->send(selection('S', id)))
        interrupt(tr("无法切换显示器。"));
}

bool DesktopSession::displayMessage(const QByteArray &msg)
{
    const char op = msg[0];
    if (state_ == Sharing && op == 'S' && msg.size() == 9) {
        const auto id = qFromBigEndian<quint64>(msg.constData() + 1);
        auto screen = findDisplay(displays_, id);
        if (screen != displays_.cend())
            rememberOnPresent_ = screen->persistentId;
        startDisplay(screen == displays_.cend() ? display_ : id);
    } else if (state_ == Sharing && op == 'R' && msg.size() == 5) {
        if (qFromBigEndian<quint32>(msg.constData() + 1) == boundary_) {
            displayReady_ = true;
            if (!rememberOnPresent_.isEmpty()) {
                store_.savePreferredDisplay(selected_, rememberOnPresent_);
                rememberOnPresent_.clear();
            }
        }
    } else if (state_ == Viewing && msg == "L") {
        announced_.clear();
        listing_ = true;
    } else if (state_ == Viewing && listing_ && op == 'd' && msg.size() >= 14 &&
               announced_.size() < displayLimit) {
        const auto id = qFromBigEndian<quint64>(msg.constData() + 1);
        QSize pixels(qFromBigEndian<quint16>(msg.constData() + 9),
                     qFromBigEndian<quint16>(msg.constData() + 11));
        if (!id || pixels.isEmpty() || findDisplay(announced_, id) != announced_.cend())
            return false;
        announced_.append({id, QString::fromUtf8(msg.mid(14)), pixels, msg[13] == 1});
    } else if (state_ == Viewing && listing_ && msg == "l" && !announced_.isEmpty()) {
        displays_ = std::move(announced_);
        listing_ = false;
        emit displaysChanged();
    } else if (state_ == Viewing && (op == 'T' || (adaptive_ && op == 't')) && msg.size() == 13) {
        const auto id = qFromBigEndian<quint64>(msg.constData() + 1);
        const bool formatOnly = op == 't';
        if (findDisplay(displays_, id) == displays_.cend() || (formatOnly && display_ != id))
            return false;
        display_ = id;
        if (!formatOnly)
            displayReady_ = false;
        needDisplayIdr_ = true;
        boundary_ = qFromBigEndian<quint32>(msg.constData() + 9);
        lastReceived_.reset();
        wanted_.reset();
        received_ = false;
        stateClock_.restart();
        if (interactive_ && !formatOnly)
            sendInput(InputEvent{});
        if (state_ == Viewing) {
            emit displaySwitched(formatOnly);
            refreshVideo();
        }
    } else
        return false;
    return true;
}

void DesktopSession::picturePresented()
{
    if (state_ != Viewing || !extended_ || displayReady_ || !display_ || needDisplayIdr_)
        return;
    QByteArray reply(5, 'R');
    qToBigEndian(boundary_, reply.data() + 1);
    if (!control_->send(reply))
        return interrupt(tr("显示器连接已断开。"));
    displayReady_ = true;
    emit displaysChanged();
}
} // namespace wwmm
