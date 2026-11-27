#include "pairing.h"
#include <QFutureWatcher>
#include <QRandomGenerator>
#include <QTimer>
#include <QtConcurrentRun>

namespace wwmm
{
Pairing::Pairing(LanProbe &probe, DeviceStore &store, QString name, int lifetimeMs)
    : probe_(probe), store_(store), name_(std::move(name)), lifetimeMs_(lifetimeMs)
{
    connect(&probe_, &LanProbe::pairingRequested, this, &Pairing::incoming);
    auto checkNetwork = [this] {
        if ((!probe_.receiving() || probe_.endpoints() != offeredOn_) && (server_ || derivingOffer_))
            cancelOffer();
        const auto links = probe_.links();
        if (inbound_ && !links.contains(*inboundLink_))
            inbound_->stop(tr("局域网接口已改变。"));
        if (outbound_ && !links.contains(*outboundLink_))
            outbound_->stop(tr("局域网接口已改变。"));
    };
    connect(&probe_, &LanProbe::endpointsChanged, this, checkNetwork);
    connect(&probe_, &LanProbe::receivingChanged, this, checkNetwork);
}

int Pairing::secondsLeft() const
{
    return server_ ? int((deadline_.remainingTime() + 999) / 1000) : 0;
}

void Pairing::offer(bool allowDirectAccess)
{
    if (derivingOffer_)
        return;
    if (store_.publicKey().isEmpty() || !probe_.receiving() || probe_.endpoints().isEmpty()) {
        emit notice(tr("请先开启接收并连接局域网。"));
        return;
    }
    cancelOffer();
    offerDirectAccess_ = allowDirectAccess;
    const auto epoch = offerEpoch_;
    offeredOn_ = probe_.endpoints();
    const auto code =
        QStringLiteral("%1").arg(QRandomGenerator::system()->bounded(100000000u), 8, 10, QChar('0'));
    derivingOffer_ = true;
    auto *job = new QFutureWatcher<PairContext>(this);
    connect(job, &QFutureWatcherBase::finished, this, [this, job, epoch, code] {
        const auto ctx = job->result();
        job->deleteLater();
        derivingOffer_ = false;
        if (epoch == offerEpoch_ && probe_.receiving() && probe_.endpoints() == offeredOn_) {
            if (ctx) {
                server_ = ctx;
                code_ = code;
                attempts_ = 5;
                deadline_ = QDeadlineTimer(lifetimeMs_, Qt::PreciseTimer);
                QTimer::singleShot(lifetimeMs_, Qt::PreciseTimer, this, [this, epoch] {
                    if (epoch == offerEpoch_) {
                        cancelOffer();
                        emit notice(tr("配对码已过期，可以生成新码。"));
                    }
                });
            } else
                emit notice(tr("无法初始化配对码。"));
        }
        emit stateChanged();
    });
    job->setFuture(
        QtConcurrent::run([code, key = store_.publicKey()] { return pairingContext(code, key, true); }));
    emit stateChanged();
}

void Pairing::clearOffer()
{
    ++offerEpoch_;
    server_.reset();
    offerDirectAccess_ = false;
    code_.clear();
    offeredOn_.clear();
    attempts_ = 0;
}
void Pairing::cancelOffer()
{
    clearOffer();
    if (inbound_)
        inbound_->stop(tr("本次配对码已失效。"));
    emit stateChanged();
}

bool Pairing::eligible(const NearbyDevice &target) const
{
    return target.key.size() == 32 && target.key != store_.publicKey() && target.endpoint.port &&
           probe_.links().contains(target.link) && isOnLink(target.endpoint.address, target.link);
}

void Pairing::incoming(QTcpSocket *socket, const LanAddress &link)
{
    if (!server_ || inbound_ || attempts_ <= 0 || deadline_.hasExpired() || !probe_.receiving() ||
        !probe_.links().contains(link) || !isOnLink(socket->peerAddress(), link)) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    --attempts_;
    const auto epoch = offerEpoch_;
    auto save = [this, epoch, link](const QByteArray &key, const QString &name, QString &error) {
        if (epoch != offerEpoch_ || deadline_.hasExpired() || !probe_.receiving() ||
            probe_.endpoints() != offeredOn_ || !probe_.links().contains(link)) {
            error = tr("本次配对码已失效。");
            return false;
        }
        if (!store_.remember(key, name, error, offerDirectAccess_))
            return false;
        clearOffer();
        return true;
    };
    auto *session = new PairSession(socket, server_, store_, name_, {}, save, this);
    inbound_ = session;
    inboundLink_ = link;
    connect(session, &PairSession::finished, this, [this](bool, const QString &message) {
        inbound_ = nullptr;
        inboundLink_.reset();
        const bool exhausted = !attempts_ && server_;
        if (exhausted)
            clearOffer();
        emit stateChanged();
        emit notice(exhausted ? tr("本次配对码已用完 5 次尝试，可以生成新码。") : message);
    });
    emit stateChanged();
    session->start();
}

void Pairing::pair(const NearbyDevice &target, const QString &code)
{
    if (busy())
        return;
    if (store_.publicKey().isEmpty() || !eligible(target)) {
        emit notice(tr("设备已离开本机直连网段。"));
        return;
    }
    derivingClient_ = true;
    const auto epoch = ++clientEpoch_;
    auto *job = new QFutureWatcher<PairContext>(this);
    connect(job, &QFutureWatcherBase::finished, this, [this, epoch, job, target] {
        const auto ctx = job->result();
        job->deleteLater();
        derivingClient_ = false;
        if (epoch != clientEpoch_ || !ctx || !eligible(target)) {
            if (epoch == clientEpoch_)
                emit notice(tr("请检查 8 位配对码和当前网络。"));
            emit stateChanged();
            return;
        }
        auto *socket = new QTcpSocket(this);
        if (!bindLanClient(*socket, target.link)) {
            socket->deleteLater();
            emit notice(tr("无法使用选定的局域网接口。"));
            emit stateChanged();
            return;
        }
        auto save = [this, target](const QByteArray &key, const QString &name, QString &error) {
            if (key != target.key || !eligible(target)) {
                error = tr("设备或网络已改变。");
                return false;
            }
            return store_.remember(key, name, error);
        };
        auto *session = new PairSession(socket, ctx, store_, name_, target.key, save, this);
        outbound_ = session;
        outboundLink_ = target.link;
        connect(session, &PairSession::finished, this, [this, target](bool ok, const QString &message) {
            outbound_ = nullptr;
            outboundLink_.reset();
            emit stateChanged();
            emit notice(message);
            if (ok)
                emit pairedWithHost(target);
        });
        connect(socket, &QTcpSocket::connected, session, &PairSession::start);
        socket->connectToHost(socketAddress(target.endpoint.address, target.link), target.endpoint.port);
        emit stateChanged();
    });
    job->setFuture(QtConcurrent::run([code, key = target.key] { return pairingContext(code, key, false); }));
    emit stateChanged();
}
void Pairing::cancelPair()
{
    ++clientEpoch_;
    if (outbound_)
        outbound_->stop(tr("已取消配对。"));
    else if (derivingClient_)
        emit notice(tr("已取消配对。"));
    emit stateChanged();
}
} // namespace wwmm
