#pragma once

#include <QHostAddress>
#include <QList>
#include <QString>
#include <optional>

class QTcpSocket;
namespace wwmm {

inline constexpr quint16 defaultProbePort = 52183;

struct LanAddress {
    QHostAddress address;
    int prefixLength;
    unsigned int interfaceIndex;
    QString interfaceName;

    bool operator==(const LanAddress &) const = default;
};

struct Endpoint {
    QHostAddress address;
    quint16 port;

    bool operator==(const Endpoint &) const = default;
};

QList<LanAddress> systemLanAddresses();
std::optional<Endpoint> parseEndpoint(QString text);
QString formatEndpoint(const Endpoint &endpoint);
bool isOnLink(const QHostAddress &peer, const LanAddress &link);
std::optional<LanAddress> selectLink(const QHostAddress &peer, const QList<LanAddress> &links);
QHostAddress socketAddress(QHostAddress address, const LanAddress &link);
bool pinToLink(qintptr descriptor, const LanAddress &link);
bool bindLanClient(QTcpSocket &socket, const LanAddress &link);

} // namespace wwmm
