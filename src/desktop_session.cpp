#include "desktop_session.h"
#include "video_link.h"
#include "video_packets.h"
#include <utility>
#include <QThread>
#include <QMutex>
#include <QtEndian>
#include <atomic>
#include <openssl/mem.h>

namespace wwmm {
namespace {
QByteArray portMessage(char op, quint16 port)
{
    QByteArray msg(3, op);
    qToBigEndian(port, msg.data() + 1);
    return msg;
}
} // namespace

struct DesktopSession::Network {
    QThread thread;
    QObject *worker = new QObject;
    VideoLink *link = nullptr;
    quint16 port = 0;
    std::atomic_int pendingSend{0}, pendingReceive{0};
    bool sendNeedsIdr = false, receiveNeedsIdr = false;
    QMutex pointerMutex;
    QByteArray pendingPointer;

    bool replacePointer(const QByteArray &value)
    {
        QMutexLocker guard(&pointerMutex);
        const bool first = pendingPointer.isEmpty();
        pendingPointer = value;
        return first;
    }

    QByteArray takePointer()
    {
        QMutexLocker guard(&pointerMutex);
        return std::exchange(pendingPointer, {});
    }

    Network(bool host, const LanAddress &local)
    {
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
        QMetaObject::invokeMethod(
            worker,
            [&] {
                link = new VideoLink(host, worker);
                port = link->bind(local);
            },
            Qt::BlockingQueuedConnection);
    }

    ~Network()
    {
        thread.quit();
        thread.wait();
    }
};

DesktopSession::DesktopSession(LanProbe &probe, DeviceStore &store, QObject *parent)
    : QObject(parent), probe_(probe), store_(store)
{
    connect(&probe, &LanProbe::desktopRequested, this, [this](QTcpSocket *socket, const LanAddress &link) {
        if (control_ || store_.peers().isEmpty() || !probe_.receiving() ||
            !isOnLink(socket->peerAddress(), link)) {
            socket->abort();
            socket->deleteLater();
            return;
        }
        attach(socket, link, {});
        control_->start();
    });
    connect(&store, &DeviceStore::changed, this, &DesktopSession::check);
    connect(&probe, &LanProbe::endpointsChanged, this, &DesktopSession::check);
    connect(&probe, &LanProbe::receivingChanged, this, &DesktopSession::check);
    stillFrame_.setSingleShot(true);
    stillFrame_.setInterval(140);
    connect(&stillFrame_, &QTimer::timeout, this, [this] {
        if (state_ == Sharing && lastSent_) {
            QByteArray frame(5, 'F');
            qToBigEndian(*lastSent_, frame.data() + 1);
            if (!control_->send(frame))
                stop(tr("桌面连接已断开。"));
        }
    });
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &DesktopSession::check);
    settlePointer_.setSingleShot(true);
    settlePointer_.setInterval(35);
    connect(&settlePointer_, &QTimer::timeout, this, [this] {
        if (state_ == Viewing && interactive_ && latestPosition_ &&
            !control_->send(packInput(*latestPosition_)))
            stop(tr("输入连接已断开。"));
    });
}

DesktopSession::~DesktopSession()
{
    if (control_)
        control_->disconnect(this);
    network_.reset();
}

QString DesktopSession::peerName() const
{
    return store_.peers().value(selected_);
}

void DesktopSession::setState(State s)
{
    state_ = s;
    stateClock_.start();
    emit changed();
}

void DesktopSession::view(const NearbyDevice &peer, bool withInput)
{
    if (control_ || (withInput && (!peer.readonlyDesktop || !peer.keyboardMouse)) ||
        !store_.peers().contains(peer.key) || !peer.endpoint.port || !probe_.links().contains(peer.link) ||
        !isOnLink(peer.endpoint.address, peer.link))
        return;
    auto *socket = new QTcpSocket(this);
    if (!bindLanClient(*socket, peer.link)) {
        socket->deleteLater();
        emit notice(tr("无法在选定的局域网接口请求桌面。"));
        return;
    }
    attach(socket, peer.link, peer.key, withInput);
    connect(socket, &QTcpSocket::connected, control_, &TrustedChannel::start);
    socket->connectToHost(socketAddress(peer.endpoint.address, peer.link), peer.endpoint.port);
}

void DesktopSession::attach(QTcpSocket *socket, const LanAddress &local, QByteArray expected, bool withInput)
{
    ++epoch_;
    host_ = expected.isEmpty();
    interactive_ = withInput;
    pointerSequence_ = pointerFence_ = lastPosition_ = 0;
    latestPosition_.reset();
    selected_ = expected;
    link_ = local;
    remote_ = {};
    received_ = false;
    lastSent_.reset();
    lastReceived_.reset();
    wanted_.reset();
    control_ = new TrustedChannel(socket, store_, std::move(expected), this);
    connect(control_, &TrustedChannel::closed, this, &DesktopSession::stop);
    connect(control_, &TrustedChannel::authenticated, this, [this, socket] {
        selected_ = control_->peerKey();
        remote_.address = socket->peerAddress();
        if (!isOnLink(remote_.address, link_) || !store_.peers().contains(selected_))
            return stop(tr("对方已离开局域网或信任已撤销。"));
        if (!host_) {
            if (!prepareNetwork())
                return stop(tr("无法准备局域网视频接收。"));
            auto request = portMessage(interactive_ ? 'C' : 'V', network_->port);
            if (interactive_)
                request.append(char(localOS()));
            if (!control_->send(request))
                return stop(tr("无法准备局域网视频接收。"));
            setState(Waiting);
        }
    });
    connect(control_, &TrustedChannel::message, this, &DesktopSession::handle);
    lastSeen_.start();
    keepalive_.start();
    timer_.start();
    setState(Connecting);
}

bool DesktopSession::prepareNetwork()
{
    network_ = std::make_unique<Network>(host_, link_);
    if (!network_->port)
        return false;
    auto *n = network_.get();
    auto e = epoch_;
    if (host_ && interactive_)
        connect(n->link, &VideoLink::pointerReceived, n->link, [this, n, e](const QByteArray &wire) {
            if (!n->replacePointer(wire))
                return;
            QMetaObject::invokeMethod(
                this,
                [this, e] {
                    if (epoch_ != e || !network_)
                        return;
                    const auto input = readInput(network_->takePointer());
                    if (input && input->kind == InputKind::Move)
                        receiveInput(*input);
                },
                Qt::QueuedConnection);
        });
    connect(
        n->link, &VideoLink::bitrateChanged, this,
        [this, e](int rate) {
            if (epoch_ == e)
                emit bitrateChanged(rate);
        },
        Qt::QueuedConnection);
    connect(
        n->link, &VideoLink::keyframeNeeded, this,
        [this, e] {
            if (epoch_ == e)
                emit keyframeNeeded();
        },
        Qt::QueuedConnection);
    connect(
        n->link, &VideoLink::failed, this,
        [this, e](const QString &error) {
            if (epoch_ == e)
                stop(error);
        },
        Qt::QueuedConnection);
    // Keep at most three compressed pictures in flight to the UI/decoder.
    connect(n->link, &VideoLink::frameReady, n->link, [this, n, e](const QByteArray &frame, quint32 stamp) {
        if (n->pendingReceive.load() >= 3) {
            n->receiveNeedsIdr = true;
            n->link->requestKeyframe();
            return;
        }
        if (n->receiveNeedsIdr) {
            bool idr = false;
            validAccessUnit(frame, idr);
            if (!idr)
                return;
            n->receiveNeedsIdr = false;
        }
        ++n->pendingReceive;
        QMetaObject::invokeMethod(
            this,
            [this, e, frame, stamp] {
                if (epoch_ != e || !network_)
                    return;
                --network_->pendingReceive;
                received_ = true;
                lastReceived_ = stamp;
                if (wanted_ && qint32(stamp - *wanted_) >= 0)
                    wanted_.reset();
                emit this->frame(frame, stamp);
            },
            Qt::QueuedConnection);
    });
    return true;
}

bool DesktopSession::startNetwork()
{
    auto keys = control_->mediaKeys();
    if (keys.size() != 60)
        return false;
    bool ok = false;
    QMetaObject::invokeMethod(
        network_->worker, [&] { ok = network_->link->connectPeer(remote_, keys); },
        Qt::BlockingQueuedConnection);
    OPENSSL_cleanse(keys.data(), keys.size());
    return ok;
}

void DesktopSession::handle(const QByteArray &data)
{
    lastSeen_.restart();
    if (data == "K")
        return;
    if (state_ == Viewing && data.size() == 5 && data[0] == 'F') {
        const auto stamp = qFromBigEndian<quint32>(data.constData() + 1);
        if (!lastReceived_ || qint32(stamp - *lastReceived_) > 0) {
            if (!wanted_)
                missingSince_.start();
            wanted_ = stamp;
            refreshVideo();
        }
        return;
    }
    if (data == "X")
        return stop(tr("对方已结束本次桌面会话。"));
    if (data == "N")
        return stop(tr("对方拒绝了本次桌面请求。"));
    if (state_ == Sharing && interactive_ && data.startsWith('I')) {
        auto input = readInput(data);
        if (!input)
            return stop(tr("对方发送了无效的输入事件。"));
        return receiveInput(*input);
    }
    if (state_ == Viewing && interactive_ && data.size() == 2 && data[0] == 'J' &&
        quint8(data[1]) <= quint8(InputFocus::Office)) {
        emit focusReceived(InputFocus(quint8(data[1])));
        return;
    }
    const bool controlOffer = data.size() == 4 && (data[0] == 'C' || data[0] == 'A');
    if ((!controlOffer && data.size() != 3) || (controlOffer && quint8(data[3]) != quint8(DesktopOS::macOS) &&
                                                quint8(data[3]) != quint8(DesktopOS::windows)))
        return stop(tr("对方发送了无效的桌面消息。"));
    remote_.port = qFromBigEndian<quint16>(data.constData() + 1);
    if (!remote_.port || !store_.peers().contains(selected_))
        return stop(tr("桌面端口或设备身份无效。"));
    if (host_ && state_ == Connecting && (data[0] == 'V' || (controlOffer && data[0] == 'C'))) {
        interactive_ = controlOffer;
        if (interactive_)
            peerOS_ = DesktopOS(quint8(data[3]));
        setState(Requested);
    } else if (!host_ && state_ == Waiting &&
               ((!interactive_ && data[0] == 'Y') || (interactive_ && controlOffer && data[0] == 'A'))) {
        if (interactive_)
            peerOS_ = DesktopOS(quint8(data[3]));
        if (!startNetwork())
            return stop(tr("无法启动加密视频接收。"));
        setState(Viewing);
    } else
        stop(tr("桌面请求状态已改变。"));
}

void DesktopSession::accept()
{
    if (state_ != Requested)
        return;
    if (!probe_.receiving() || !probe_.links().contains(link_) || !store_.peers().contains(selected_) ||
        !prepareNetwork() || !startNetwork())
        return stop(tr("无法在当前局域网共享桌面。"));
    auto reply = portMessage(interactive_ ? 'A' : 'Y', network_->port);
    if (interactive_)
        reply.append(char(localOS()));
    if (!control_->send(reply))
        return stop(tr("无法在当前局域网共享桌面。"));
    setState(Sharing);
}

void DesktopSession::sendInput(InputEvent input)
{
    if (state_ != Viewing || !interactive_ || !network_)
        return;
    if (input.kind == InputKind::Move) {
        input.sequence = ++pointerSequence_;
        input.fence = pointerFence_;
        latestPosition_ = input;
        settlePointer_.start();
        auto *n = network_.get();
        if (n->replacePointer(packInput(input)))
            QMetaObject::invokeMethod(
                n->link, [n] { n->link->sendPointer(n->takePointer()); }, Qt::QueuedConnection);
        return;
    }
    if (input.kind == InputKind::Reset || input.kind == InputKind::ButtonDown ||
        input.kind == InputKind::ButtonUp || input.kind == InputKind::Wheel) {
        input.sequence = pointerFence_ = ++pointerSequence_;
        settlePointer_.stop();
        latestPosition_.reset();
    }
    if (input.kind == InputKind::Text) {
        for (const auto &part : inputTextChunks(input.text)) {
            input.text = part;
            if (!control_->send(packInput(input)))
                return stop(tr("文字输入连接已断开。"));
        }
    } else if (!control_->send(packInput(input)))
        stop(tr("输入连接已断开。"));
}

void DesktopSession::receiveInput(const InputEvent &input)
{
    if (state_ != Sharing || !interactive_)
        return;
    if (input.kind == InputKind::Move) {
        if (input.fence != pointerFence_ || !newerInput(input.sequence, lastPosition_))
            return;
        lastPosition_ = input.sequence;
    } else if (input.kind == InputKind::ButtonDown || input.kind == InputKind::ButtonUp ||
               input.kind == InputKind::Wheel || input.kind == InputKind::Reset) {
        if (!newerInput(input.sequence, pointerFence_))
            return;
        pointerFence_ = lastPosition_ = input.sequence;
    }
    emit inputReceived(input);
}

void DesktopSession::reportFocus(InputFocus focus)
{
    if (state_ == Sharing && interactive_ && !control_->send(QByteArray("J", 1).append(char(focus))))
        stop(tr("输入连接已断开。"));
}

void DesktopSession::sendFrame(QByteArray frame, quint32 stamp, bool key)
{
    if (state_ != Sharing || !network_)
        return;
    lastSent_ = stamp;
    stillFrame_.start();
    auto *n = network_.get();
    if (n->pendingSend.load() >= 3) {
        n->sendNeedsIdr = true;
        emit keyframeNeeded();
        return;
    }
    if (n->sendNeedsIdr && !key)
        return;
    n->sendNeedsIdr = false;
    ++n->pendingSend;
    QMetaObject::invokeMethod(
        n->link,
        [n, frame = std::move(frame), stamp] {
            --n->pendingSend;
            n->link->submit(frame, stamp);
        },
        Qt::QueuedConnection);
}

void DesktopSession::refreshVideo()
{
    if (state_ == Viewing && network_)
        QMetaObject::invokeMethod(network_->link, &VideoLink::requestKeyframe, Qt::QueuedConnection);
}

void DesktopSession::check()
{
    if (!control_)
        return;
    if ((host_ && !probe_.receiving()) || !probe_.links().contains(link_) ||
        (!selected_.isEmpty() && !store_.peers().contains(selected_)))
        return stop(tr("设备信任、接收开关或本机网络已改变。"));
    if (state_ == Connecting && stateClock_.elapsed() > 7000)
        return stop(tr("桌面连接超时。"));
    if ((state_ == Waiting || state_ == Requested) && stateClock_.elapsed() > 45000)
        return stop(tr("本次桌面请求已超时。"));
    if (state_ == Connecting)
        return;
    if (lastSeen_.elapsed() > (interactive_ ? 4500 : 14000))
        return stop(tr("局域网桌面连接已超时。"));
    if (keepalive_.elapsed() >= (interactive_ ? 900 : 2500)) {
        if (!control_->send("K"))
            return stop(tr("桌面连接已断开。"));
        keepalive_.restart();
    }
    if (state_ == Viewing && wanted_ && received_) {
        refreshVideo();
        if (missingSince_.elapsed() > 12000)
            return stop(tr("无法更新桌面，请检查局域网 UDP 连接。"));
    }
    if (state_ == Viewing && !received_) {
        refreshVideo();
        if (stateClock_.elapsed() > 15000)
            stop(tr("未收到桌面画面，请检查共享端的录屏权限和局域网 UDP。"));
    }
}

void DesktopSession::stop(const QString &reason)
{
    if (!control_)
        return;
    ++epoch_;
    timer_.stop();
    stillFrame_.stop();
    settlePointer_.stop();
    latestPosition_.reset();
    auto *channel = std::exchange(control_, nullptr);
    channel->disconnect(this);
    channel->finish(state_ == Requested && reason.isEmpty() ? "N" : "X");
    network_.reset();
    selected_.clear();
    interactive_ = false;
    setState(Idle);
    emit notice(reason.isEmpty() ? tr("本次桌面会话已结束。") : reason);
}
} // namespace wwmm
