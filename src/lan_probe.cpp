#include "lan_probe.h"

#include <QNetworkProxy>
#include <QMetaMethod>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace wwmm {
namespace {
constexpr int timeoutMs = 3000;
constexpr int maxInbound = 16;
}

LanProbe::LanProbe(NetworkProvider networks, quint16 port, DesktopOS os, QObject *parent)
    : QObject(parent), networks_(std::move(networks)), preferredPort_(port), os_(os)
{
    auto *watch = new QTimer(this);
    watch->setInterval(5000);
    watch->setTimerType(Qt::VeryCoarseTimer);
    connect(watch, &QTimer::timeout, this, &LanProbe::refresh);
    watch->start();
}

LanProbe::~LanProbe()
{
    if (outgoing_) {
        outgoing_->disconnect(this);
        outgoing_->abort();
    }
    for (auto *socket : inbound_.keys())
        closeInbound(socket);
    for (const auto &listener : std::as_const(listeners_))
        listener.server->close();
}

QList<LocalEndpoint> LanProbe::endpoints() const
{
    QList<LocalEndpoint> result;
    for (const auto &listener : listeners_)
        result.append({listener.link, listener.server->serverPort()});
    return result;
}

void LanProbe::setReceiving(bool enabled)
{
    if (receiving_ == enabled)
        return;
    receiving_ = enabled;
    refresh();
    emit receivingChanged(enabled);
}

void LanProbe::refresh()
{
    if (!receiving_ && listeners_.isEmpty() && listenIssue_.isEmpty() && !busy() &&
        !isSignalConnected(QMetaMethod::fromSignal(&LanProbe::networkRefreshed)))
        return;
    const auto links = networks_();
    if (outgoingLink_ && !links.contains(*outgoingLink_))
        fail(tr("本机网络已改变，请重新检测。"));
    const auto previous = endpoints();
    const auto previousIssue = listenIssue_;
    for (auto *socket : inbound_.keys()) {
        if (!receiving_ || !links.contains(inbound_.value(socket)))
            closeInbound(socket);
    }
    listeners_.removeIf([&](const Listener &entry) {
        if (receiving_ && links.contains(entry.link))
            return false;
        entry.server->close();
        entry.server->deleteLater();
        return true;
    });

    listenIssue_.clear();
    if (receiving_) {
        if (links.isEmpty())
            listenIssue_ = tr("未找到已连接的有线或 Wi-Fi 接口。");
        for (const auto &link : links) {
            if (std::any_of(listeners_.cbegin(), listeners_.cend(),
                            [&](const Listener &entry) { return entry.link == link; }))
                continue;
            auto *server = new QTcpServer(this);
            server->setProxy(QNetworkProxy::NoProxy);
            server->setMaxPendingConnections(maxInbound);
            const auto local = socketAddress(link.address, link);
            bool listening = server->listen(local, preferredPort_);
            if (!listening && preferredPort_ && server->serverError() == QAbstractSocket::AddressInUseError)
                listening = server->listen(local, 0);
            if (!listening || !pinToLink(server->socketDescriptor(), link)) {
                listenIssue_ = tr("无法在 %1 上接收检测。").arg(link.interfaceName);
                delete server;
                continue;
            }
            const Listener entry{link, server};
            listeners_.append(entry);
            connect(server, &QTcpServer::newConnection, this, [this, entry] { acceptConnections(entry); });
        }
    }
    if (endpoints() != previous || listenIssue_ != previousIssue)
        emit endpointsChanged();
    emit networkRefreshed(links);
}

void LanProbe::acceptConnections(const Listener &listener)
{
    while (auto *socket = listener.server->nextPendingConnection()) {
        socket->setParent(this);
        const auto link = listener.link;
        if (!receiving_ || inbound_.size() >= maxInbound || !isOnLink(socket->peerAddress(), link)
            || !pinToLink(socket->socketDescriptor(), link)) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        inbound_.insert(socket, link);
        socket->setReadBufferSize(probeMessageSize + 1);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { closeInbound(socket); });
        QTimer::singleShot(timeoutMs, socket, [this, socket] { closeInbound(socket); });
        auto readRequest = [this, socket, link] {
            const auto prefix = socket->peek(4);
            if (prefix == "WMP2" || prefix == "WMV3") {
                const auto receiver = prefix == "WMP2" ? &LanProbe::pairingRequested : &LanProbe::desktopRequested;
                if (!receiving_ || !networks_().contains(link)
                    || !isSignalConnected(QMetaMethod::fromSignal(receiver))) {
                    closeInbound(socket);
                    return;
                }
                socket->read(4);
                inbound_.remove(socket);
                socket->disconnect(this);
                socket->setReadBufferSize(65536);
                (this->*receiver)(socket, link);
                return;
            }
            if (socket->bytesAvailable() < probeMessageSize)
                return;
            const auto reply = probeReply(socket->readAll(), os_);
            if (reply.isEmpty() || !receiving_ || !networks_().contains(link)) {
                closeInbound(socket);
                return;
            }
            const auto peer = socket->peerAddress();
            disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
            if (socket->write(reply) != reply.size()) {
                closeInbound(socket);
                return;
            }
            socket->disconnectFromHost();
            emit received(peer);
        };
        connect(socket, &QTcpSocket::readyRead, this, readRequest);
        readRequest();
    }
}

void LanProbe::closeInbound(QTcpSocket *socket)
{
    if (!inbound_.remove(socket))
        return;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
}

void LanProbe::check(const QString &destination)
{
    if (busy())
        return;
    const auto endpoint = parseEndpoint(destination);
    if (!endpoint) {
        emit checkFailed(tr("请输入有效的 IPv4 或 IPv6 地址。端口范围为 1–65535。"));
        return;
    }
    const auto link = selectLink(endpoint->address, networks_());
    if (!link) {
        emit checkFailed(tr("目标须在本机直连网段内。多个接口重叠时，IPv6 可用 %接口 指定本机接口。"));
        return;
    }
    auto *socket = new QTcpSocket(this);
    socket->setReadBufferSize(probeMessageSize + 1);
    if (!bindLanClient(*socket, *link)) {
        socket->deleteLater();
        emit checkFailed(tr("无法使用选定的局域网接口，请刷新本机地址。"));
        return;
    }
    const auto nonce = QUuid::createUuid().toRfc4122();
    outgoing_ = socket;
    outgoingLink_ = link;
    connect(socket, &QTcpSocket::connected, this, [this, socket, nonce] {
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        if (socket->write(probeRequest(nonce)) != probeMessageSize)
            fail(tr("无法发送检测请求。"));
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket, nonce] {
        if (socket->bytesAvailable() < probeMessageSize)
            return;
        const auto remote = readProbeReply(socket->readAll(), nonce);
        const auto elapsed = clock_.elapsed();
        if (!remote) {
            fail(tr("对方未返回有效的 WWMM Remote 检测响应。"));
            return;
        }
        if (!networks_().contains(*outgoingLink_)) {
            fail(tr("本机网络已改变，请重新检测。"));
            return;
        }
        clearOutgoing();
        emit checkSucceeded(*remote, elapsed);
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError error) {
        if (error == QAbstractSocket::ConnectionRefusedError)
            fail(tr("对方拒绝连接，请核对地址、端口和接收开关。"));
        else
            fail(tr("连接失败：%1").arg(socket->errorString()));
    });
    QTimer::singleShot(timeoutMs, socket, [this, socket] {
        if (outgoing_ == socket)
            fail(tr("检测超时，请检查对方是否在线以及系统是否允许局域网连接。"));
    });
    clock_.start();
    emit busyChanged(true);
    if (outgoing_ == socket)
        socket->connectToHost(socketAddress(endpoint->address, *link), endpoint->port);
}

void LanProbe::clearOutgoing()
{
    auto *socket = std::exchange(outgoing_, nullptr);
    outgoingLink_.reset();
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    emit busyChanged(false);
}

void LanProbe::fail(const QString &reason)
{
    clearOutgoing();
    emit checkFailed(reason);
}

void LanProbe::cancel()
{
    if (busy()) {
        clearOutgoing();
        emit checkFailed(tr("已取消检测。"));
    }
}

} // namespace wwmm
