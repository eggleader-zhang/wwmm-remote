#pragma once

#include "discovery.h"
#include "pair_session.h"
#include <QDeadlineTimer>

namespace wwmm
{
class Pairing final : public QObject
{
    Q_OBJECT
public:
    Pairing(LanProbe &probe, DeviceStore &store, QString name, int lifetimeMs = 120000);
    void offer();
    void cancelOffer();
    void pair(const NearbyDevice &target, const QString &code);
    void cancelPair();
    bool preparing() const
    {
        return derivingOffer_;
    }
    bool busy() const
    {
        return derivingClient_ || outbound_;
    }
    QString code() const
    {
        return code_;
    }
    int secondsLeft() const;
    int attemptsLeft() const
    {
        return attempts_;
    }
signals:
    void stateChanged();
    void notice(const QString &message);

private:
    void incoming(QTcpSocket *socket, const LanAddress &link);
    void clearOffer();
    bool eligible(const NearbyDevice &target) const;
    LanProbe &probe_;
    DeviceStore &store_;
    QString name_, code_;
    const int lifetimeMs_;
    PairContext server_;
    PairSession *inbound_ = nullptr;
    PairSession *outbound_ = nullptr;
    std::optional<LanAddress> inboundLink_, outboundLink_;
    QDeadlineTimer deadline_;
    QList<LocalEndpoint> offeredOn_;
    int attempts_ = 0;
    quint64 offerEpoch_ = 0, clientEpoch_ = 0;
    bool derivingOffer_ = false, derivingClient_ = false;
};
} // namespace wwmm
