#pragma once

#include "lan_address.h"
#include "probe_message.h"
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <functional>

class QTcpServer;
class QTcpSocket;

namespace wwmm {

struct LocalEndpoint {
    LanAddress link;
    quint16 port;
    bool operator==(const LocalEndpoint &) const = default;
};

class LanProbe final : public QObject {
    Q_OBJECT
public:
    using NetworkProvider = std::function<QList<LanAddress>()>;
    explicit LanProbe(NetworkProvider networks = systemLanAddresses, quint16 port = defaultProbePort,
                      DesktopOS os = localOS(), QObject *parent = nullptr);
    ~LanProbe() override;
    QList<LocalEndpoint> endpoints() const;
    QList<LanAddress> links() const { return networks_(); }
    QString listenIssue() const { return listenIssue_; }
    bool receiving() const { return receiving_; }
    bool busy() const { return outgoing_ != nullptr; }
    void setReceiving(bool enabled);
    void refresh();
    void check(const QString &destination);
    void cancel();

signals:
    void networkRefreshed(const QList<wwmm::LanAddress> &links);
    void endpointsChanged();
    void receivingChanged(bool enabled);
    void busyChanged(bool busy);
    void checkSucceeded(wwmm::DesktopOS os, qint64 elapsedMs);
    void checkFailed(const QString &reason);
    void received(const QHostAddress &peer);
    void pairingRequested(QTcpSocket *socket, const wwmm::LanAddress &link);
    void desktopRequested(QTcpSocket *socket, const wwmm::LanAddress &link);

private:
    struct Listener {
        LanAddress link;
        QTcpServer *server;
    };
    void acceptConnections(const Listener &listener);
    void closeInbound(QTcpSocket *socket);
    void clearOutgoing();
    void fail(const QString &reason);

    NetworkProvider networks_;
    quint16 preferredPort_;
    DesktopOS os_;
    bool receiving_ = false;
    QList<Listener> listeners_;
    QString listenIssue_;
    QHash<QTcpSocket *, LanAddress> inbound_;
    QTcpSocket *outgoing_ = nullptr;
    std::optional<LanAddress> outgoingLink_;
    QElapsedTimer clock_;
};

} // namespace wwmm
