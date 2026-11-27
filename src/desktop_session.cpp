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

DesktopSession::DesktopSession(LanProbe &probe, DeviceStore &store, std::function<bool(QString &)> allowInput,
                               QObject *parent)
    : QObject(parent), probe_(probe), store_(store), allowInput_(std::move(allowInput)), data_(store, this)
{
    connect(&probe, &LanProbe::desktopRequested, this, [this](QTcpSocket *socket, const LanAddress &link) {
        if (installing_ || state_ != Idle || store_.peers().isEmpty() || !probe_.receiving() ||
            !isOnLink(socket->peerAddress(), link)) {
            socket->abort();
            socket->deleteLater();
            return;
        }
        attach(socket, link, {});
        control_->start();
    });
    pasteTimer_.setSingleShot(true);
    pasteTimer_.setInterval(3000);
    connect(&pasteTimer_, &QTimer::timeout, this, [this] {
        if (!afterPaste_.isEmpty()) {
            sendInput(InputEvent{});
            emit notice(tr("剪贴板尚未同步，本次粘贴已取消。"));
        }
    });
    connect(&data_, &SessionData::clipboardRejected, this, [this] {
        imagePaste_.reset();
        if (!afterPaste_.isEmpty())
            sendInput(InputEvent{});
    });
    connect(&data_, &SessionData::clipboardSettled, this, &DesktopSession::drainPaste);
    connect(&data_, &SessionData::notice, this, &DesktopSession::notice);
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
                interrupt(tr("桌面连接已断开。"));
        }
    });
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &DesktopSession::check);
    settlePointer_.setSingleShot(true);
    settlePointer_.setInterval(35);
    connect(&settlePointer_, &QTimer::timeout, this, [this] {
        if (state_ == Viewing && interactive_ && latestPosition_ &&
            !control_->send(packInput(*latestPosition_)))
            interrupt(tr("输入连接已断开。"));
    });
}

DesktopSession::~DesktopSession()
{
    data_.disconnect();
    data_.reset();
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
    if (installing_ || control_ || (state_ == Interrupted && (peer.key != selected_ || withInput != interactive_)))
        return;
    if (!peer.canRequestDesktop(withInput)) {
        emit notice(tr("对方不支持所选的桌面模式。"));
        return;
    }
    if (!store_.peers().contains(peer.key)) {
        emit notice(tr("设备的配对记录已改变。请重新选中设备。"));
        return;
    }
    if (!peer.endpoint.port || !probe_.links().contains(peer.link) || !isOnLink(peer.endpoint.address, peer.link)) {
        emit notice(tr("对方的局域网地址已失效。设备重新上线后请再连接。"));
        return;
    }
    auto *socket = new QTcpSocket(this);
    if (!bindLanClient(*socket, peer.link)) {
        socket->deleteLater();
        emit notice(tr("无法在选定的局域网接口请求桌面。"));
        return;
    }
    extended_ = peer.extendedDesktop;
    rich_ = extended_ && peer.richMedia;
    adaptive_ = rich_ && peer.adaptiveVideo;
    macKeyboardProtocol_ = adaptive_ && withInput && localOS() == DesktopOS::windows &&
                           peer.os == DesktopOS::macOS && peer.macKeyboard;
    retrying_ = state_ == Interrupted;
    attach(socket, peer.link, peer.key, withInput);
    connect(socket, &QTcpSocket::connected, control_, &TrustedChannel::start);
    socket->connectToHost(socketAddress(peer.endpoint.address, peer.link), peer.endpoint.port);
}

void DesktopSession::attach(QTcpSocket *socket, const LanAddress &local, QByteArray expected, bool withInput)
{
    ++epoch_;
    host_ = expected.isEmpty();
    interactive_ = withInput;
    if (host_)
        macKeyboardProtocol_ = adaptive_ = rich_ = extended_ = false;
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
    connect(control_, &TrustedChannel::closed, this, &DesktopSession::finishSession);
    connect(control_, &TrustedChannel::authenticated, this, [this, socket] {
        selected_ = control_->peerKey();
        remote_.address = socket->peerAddress();
        if (!isOnLink(remote_.address, link_) || !store_.peers().contains(selected_))
            return stop(tr("对方已离开局域网或信任已撤销。"));
        if (!host_) {
            if (!prepareNetwork())
                return stop(tr("无法准备局域网视频接收。"));
            const char op = interactive_ ? (extended_ ? 'c' : 'C') : (extended_ ? 'v' : 'V');
            auto request = portMessage(op, network_->port);
            if (interactive_)
                request.append(char(localOS()));
            if (rich_)
                request.append(char(macKeyboardProtocol_ ? 15 : adaptive_ ? 14 : 13));
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
    if (host_ && adaptive_)
        connect(n->link, &VideoLink::receptionSample, this,
                [this, e](qint64 now, int loss, int jitter, bool queued) {
                    if (epoch_ != e || state_ != Sharing || !displayReady_)
                        return;
                    const auto next = resolution_.observe(quality_, qMin(qualityLimit_, hardwareLimit_),
                                                          now, loss, jitter, queued);
                    if (next != quality_)
                        applyQuality(next, true);
                }, Qt::QueuedConnection);
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
    connect(n->link, &VideoLink::transportLost, this, [this, e](const QString &reason) {
        if (epoch_ == e)
            interrupt(reason);
    }, Qt::QueuedConnection);
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
                if (extended_ && (!display_ || needDisplayIdr_)) {
                    bool idr = false;
                    if (!display_ || qint32(stamp - boundary_) < 0 || !validAccessUnit(frame, idr) || !idr)
                        return;
                    needDisplayIdr_ = false;
                }
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
    auto audioKey = rich_ ? control_->mediaKeys(true) : QByteArray{};
    if (rich_ && audioKey.size() != 30)
        return false;
    bool ok = false;
    QMetaObject::invokeMethod(
        network_->worker, [&] { ok = network_->link->connectPeer(remote_, keys, audioKey); },
        Qt::BlockingQueuedConnection);
    OPENSSL_cleanse(keys.data(), keys.size());
    if (!audioKey.isEmpty())
        OPENSSL_cleanse(audioKey.data(), audioKey.size());
    return ok;
}

void DesktopSession::handle(const QByteArray &data)
{
    lastSeen_.restart();
    if (data == "K")
        return;
    if (extended_ && (state_ == Sharing || state_ == Viewing)) {
        if (rich_ && mediaMessage(data))
            return;
        if (state_ == Viewing && interactive_ && data.size() == 19 && data[0] == 'B') {
            data_.join(link_, remote_.address, selected_, data, rich_);
            return;
        }
        if (displayMessage(data))
            return;
    }
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
    if (data == "D" && adaptive_ && (state_ == Viewing || state_ == Sharing || state_ == Waiting))
        return interrupt(tr("对方的局域网连接已中断。"));
    if (data == "N")
        return stop(tr("对方拒绝了本次桌面请求。"));
    if (state_ == Sharing && interactive_ && data.startsWith('I')) {
        auto input = readInput(data);
        if (!input || (input->macKeyboard && !macKeyboardProtocol_))
            return stop(tr("对方发送了无效的输入事件。"));
        return receiveInput(*input);
    }
    if (state_ == Viewing && interactive_ && data.size() == 2 && data[0] == 'J' &&
        quint8(data[1]) <= quint8(InputFocus::Office)) {
        emit focusReceived(InputFocus(quint8(data[1])));
        return;
    }
    const char op = data[0];
    const bool modern = op == 'v' || op == 'y' || op == 'c' || op == 'a';
    const bool controlOffer = op == 'C' || op == 'A' || op == 'c' || op == 'a';
    const int baseSize = controlOffer ? 4 : 3;
    // v15 extends the adaptive control handshake only for a Windows viewer and a Mac host.
    const bool macKeysOffer = modern && controlOffer && data.size() == baseSize + 1 && quint8(data.back()) == 15 &&
        ((host_ && localOS() == DesktopOS::macOS && quint8(data[3]) == quint8(DesktopOS::windows)) ||
         (!host_ && localOS() == DesktopOS::windows && quint8(data[3]) == quint8(DesktopOS::macOS)));
    const bool adaptiveOffer = macKeysOffer || (modern && data.size() == baseSize + 1 && quint8(data.back()) == 14);
    const bool richOffer = adaptiveOffer || (modern && data.size() == baseSize + 1 && quint8(data.back()) == 13);
    if ((!richOffer && data.size() != baseSize) ||
        (controlOffer && quint8(data[3]) != quint8(DesktopOS::macOS) &&
         quint8(data[3]) != quint8(DesktopOS::windows)))
        return stop(tr("对方发送了无效的桌面消息。"));
    remote_.port = qFromBigEndian<quint16>(data.constData() + 1);
    if (!remote_.port || !store_.peers().contains(selected_))
        return stop(tr("桌面端口或设备身份无效。"));
    if (host_ && state_ == Connecting &&
        (op == 'V' || op == 'v' || (controlOffer && (op == 'C' || op == 'c')))) {
        extended_ = modern;
        rich_ = richOffer;
        adaptive_ = adaptiveOffer;
        macKeyboardProtocol_ = macKeysOffer;
        interactive_ = controlOffer;
        if (interactive_)
            peerOS_ = DesktopOS(quint8(data[3]));
        if (store_.allowsDirectAccess(selected_)) {
            usingDirectAccess_ = true;
            startSharing(false);
        } else {
            setState(Requested);
        }
    } else if (!host_ && state_ == Waiting && modern == extended_ && richOffer == rich_ && adaptiveOffer == adaptive_ &&
               macKeysOffer == macKeyboardProtocol_ &&
               ((!interactive_ && (op == 'Y' || op == 'y')) ||
                (interactive_ && controlOffer && (op == 'A' || op == 'a')))) {
        if (interactive_)
            peerOS_ = DesktopOS(quint8(data[3]));
        if (!startNetwork())
            return stop(tr("无法启动加密视频接收。"));
        retrying_ = false;
        setState(Viewing);
    } else
        stop(tr("桌面请求状态已改变。"));
}

void DesktopSession::accept(bool rememberDevice)
{
    if (state_ == Requested)
        startSharing(rememberDevice && interactive_);
}

void DesktopSession::startSharing(bool rememberDevice)
{
    if (!probe_.receiving() || !probe_.links().contains(link_) || !store_.peers().contains(selected_))
        return stop(tr("无法在当前局域网共享桌面。"));
    QString reason;
    if (interactive_ && !allowInput_(reason))
        return stop(reason);
    if (!prepareNetwork() || !startNetwork())
        return stop(tr("无法在当前局域网共享桌面。"));
    if (rememberDevice) {
        if (!store_.setDirectAccess(selected_, true, reason))
            return stop(reason);
        usingDirectAccess_ = true;
    }
    const char op = interactive_ ? (extended_ ? 'a' : 'A') : (extended_ ? 'y' : 'Y');
    auto reply = portMessage(op, network_->port);
    if (interactive_)
        reply.append(char(localOS()));
    if (rich_)
        reply.append(char(macKeyboardProtocol_ ? 15 : adaptive_ ? 14 : 13));
    if (!control_->send(reply))
        return stop(tr("无法在当前局域网共享桌面。"));
    setState(Sharing);
    if (state_ == Sharing && extended_ && interactive_) {
        const auto offer = data_.listen(link_, remote_.address, selected_, rich_);
        if (!offer.isEmpty() && !control_->send(offer))
            interrupt(tr("桌面连接已断开。"));
    }
}

void DesktopSession::setAutoMacKeys(bool enabled)
{
    if (autoMacKeys_ == enabled)
        return;
    if (state_ == Viewing && macKeyboardProtocol_)
        sendInput(InputEvent{});
    autoMacKeys_ = enabled;
    emit macKeyboardChanged(usesMacKeyboard());
}

void DesktopSession::sendInput(InputEvent input)
{
    if (state_ != Viewing || !interactive_ || !network_ ||
        (extended_ && !displayReady_ && input.kind != InputKind::Reset))
        return;
    data_.noteActivity();
    input.macKeyboard = usesMacKeyboard() && input.carriesModifiers();
    if (input.kind == InputKind::Reset) {
        imagePaste_.reset();
        pasteTimer_.stop();
        afterPaste_.clear();
    } else {
        const bool paste = input.isPasteShortcut(localOS());
        if (paste)
            emit beforePaste();
        if (paste && data_.imagePending()) {
            if (!imagePaste_)
                sendInput(InputEvent{});
            if (!control_)
                return;
            imagePaste_ = input;
            imagePasteHeldMods_ = input.mods;
            pasteRevision_ = data_.clipboardRevisionId();
            return;
        }
        // Keep typing and pointer traffic live while a large screenshot is in flight.
        // An action that changes the insertion target cancels a deferred paste.
        if (imagePaste_ && input.kind != InputKind::Move) {
            if (input.kind == InputKind::KeyUp && (input.key == imagePaste_->key || modifierBit(input.key)))
                imagePasteHeldMods_ = input.mods;
            else
                imagePaste_.reset();
        }
        if ((paste && data_.clipboardPending()) || !afterPaste_.isEmpty()) {
            if (input.kind == InputKind::Move)
                return;
            if (afterPaste_.size() < 128) {
                if (afterPaste_.isEmpty())
                    pasteTimer_.start();
                afterPaste_.append(input);
                return;
            }
            return sendInput(InputEvent{});
        }
    }
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
                return interrupt(tr("文字输入连接已断开。"));
        }
    } else if (!control_->send(packInput(input)))
        interrupt(tr("输入连接已断开。"));
}

void DesktopSession::receiveInput(const InputEvent &input)
{
    if (state_ != Sharing || !interactive_ || (extended_ && !displayReady_ && input.kind != InputKind::Reset))
        return;
    data_.noteActivity();
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

void DesktopSession::drainPaste()
{
    if (data_.clipboardPending())
        return;
    pasteTimer_.stop();
    const auto imagePaste = std::exchange(imagePaste_, {});
    if (imagePaste && data_.clipboardEnabled() && pasteRevision_ == data_.clipboardRevisionId() &&
        state_ == Viewing && displayReady_) {
        auto release = *imagePaste;
        release.kind = InputKind::KeyUp;
        release.mods = imagePaste->macKeyboard ? imagePasteHeldMods_ : 0;
        if (!control_->send(packInput(*imagePaste)) || !control_->send(packInput(release)))
            return interrupt(tr("截图粘贴连接已断开。"));
    }
    const auto pending = std::exchange(afterPaste_, {});
    if (!pending.isEmpty() && !data_.clipboardEnabled())
        return sendInput(InputEvent{});
    for (const auto &event : pending)
        sendInput(event);
}

void DesktopSession::reportFocus(InputFocus focus)
{
    if (state_ == Sharing && interactive_ && !control_->send(QByteArray("J", 1).append(char(focus))))
        interrupt(tr("输入连接已断开。"));
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

void DesktopSession::attachSound()
{
    if (!network_)
        return;
    auto *link = network_->link;
    QMetaObject::invokeMethod(
        link, [link, stream = sound_] { link->setAudio(stream); }, Qt::QueuedConnection);
}

void DesktopSession::applyQuality(VideoQuality quality, bool keepEstimate)
{
    if (state_ != Sharing || !rich_ || !display_ || !network_)
        return;
    const bool changed = quality != quality_;
    quality_ = quality;
    auto message = QByteArray("q") + char(quality);
    if (adaptive_)
        message.append(char(qualityLimit_));
    if (!control_->send(message))
        return interrupt(tr("画质连接已断开。"));
    emit qualityChanged();
    if (changed || !adaptive_) {
        auto *link = network_->link;
        QMetaObject::invokeMethod(link, [link, quality, keepEstimate] { link->setQuality(quality, keepEstimate); },
                                  Qt::QueuedConnection);
        startDisplay(display_, adaptive_ && displayReady_);
    }
}

void DesktopSession::refreshVideo()
{
    if (state_ == Viewing && network_)
        QMetaObject::invokeMethod(network_->link, &VideoLink::requestKeyframe, Qt::QueuedConnection);
}

void DesktopSession::check()
{
    if (usingDirectAccess_ && !store_.allowsDirectAccess(selected_))
        return stop(tr("已撤销此设备的免确认访问，本次共享已结束。"));
    if ((!selected_.isEmpty() && !store_.peers().contains(selected_)) || (host_ && !probe_.receiving()))
        return stop(tr("设备信任或接收开关已改变。"));
    if (!control_)
        return;
    if (!probe_.links().contains(link_))
        return interrupt(tr("本机局域网接口已改变。"));
    if (state_ == Connecting && stateClock_.elapsed() > 7000)
        return interrupt(tr("桌面连接超时。"));
    if ((state_ == Waiting || state_ == Requested) && stateClock_.elapsed() > 45000)
        return stop(tr("本次桌面请求已超时。"));
    if (state_ == Connecting)
        return;
    if (lastSeen_.elapsed() > (interactive_ ? 4500 : 14000))
        return interrupt(tr("局域网桌面连接已超时。"));
    if (keepalive_.elapsed() >= (interactive_ ? 900 : 2500)) {
        if (!control_->send("K"))
            return interrupt(tr("桌面连接已断开。"));
        keepalive_.restart();
    }
    if (state_ == Viewing && wanted_ && received_) {
        refreshVideo();
        if (missingSince_.elapsed() > 12000)
            return interrupt(tr("无法更新桌面，请检查局域网 UDP 连接。"));
    }
    if (state_ == Viewing && !received_) {
        refreshVideo();
        if (rich_ && quality_ > (adaptive_ ? VideoQuality::HD : VideoQuality::FullHD) && stateClock_.elapsed() > 6500) {
            stateClock_.restart();
            reduceQuality();
            return;
        }
        if (stateClock_.elapsed() > 15000)
            stop(tr("未收到桌面画面，请检查共享端的录屏权限和局域网 UDP。"));
    }
}

void DesktopSession::stop(const QString &reason)
{
    finishSession(reason, false);
}

void DesktopSession::interrupt(const QString &reason)
{
    finishSession(reason, true);
}

void DesktopSession::finishSession(const QString &reason, bool transportLost)
{
    if (!control_ && (state_ != Interrupted || transportLost))
        return;
    const bool resume = transportLost && !host_ && (state_ == Viewing || retrying_) &&
                        store_.peers().contains(selected_);
    const auto last = transportLost && adaptive_ ? "D" : (state_ == Requested && reason.isEmpty() ? "N" : "X");
    auto *channel = std::exchange(control_, nullptr);
    if (channel) {
        channel->disconnect(this);
        channel->finish(last);
    }
    ++epoch_;
    usingDirectAccess_ = false;
    timer_.stop();
    pasteTimer_.stop();
    stillFrame_.stop();
    settlePointer_.stop();
    latestPosition_.reset();
    imagePaste_.reset();
    afterPaste_.clear();
    data_.reset();
    displays_.clear();
    announced_.clear();
    display_ = 0;
    rememberOnPresent_.clear();
    macKeyboardProtocol_ = adaptive_ = rich_ = extended_ = displayReady_ = needDisplayIdr_ = listing_ = false;
    qualityLimit_ = quality_ = VideoQuality::FullHD;
    hardwareLimit_ = VideoQuality::UHD;
    resolution_ = {};
    localSound_ = peerSound_ = 0;
    sound_.reset();
    network_.reset();
    if (!resume) {
        selected_.clear();
        interactive_ = false;
    }
    retrying_ = resume;
    setState(resume ? Interrupted : Idle);
    emit notice(reason.isEmpty() ? tr("本次桌面会话已结束。") : reason);
}
} // namespace wwmm
