#include "lan_address.h"

#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace wwmm {
namespace {

bool isV4(const QHostAddress &address)
{
    return address.protocol() == QAbstractSocket::IPv4Protocol;
}

QHostAddress unscoped(QHostAddress address)
{
    address.setScopeId({});
    return address;
}

bool isUnicast(const QHostAddress &address)
{
    if (isV4(address)) {
        const quint32 first = address.toIPv4Address() >> 24;
        return first > 0 && first < 224;
    }
    if (address.protocol() != QAbstractSocket::IPv6Protocol
        || address == QHostAddress::AnyIPv6 || address.isMulticast())
        return false;
    const auto bytes = address.toIPv6Address();
    const bool mapped = std::all_of(bytes.c, bytes.c + 10, [](quint8 b) { return b == 0; })
                        && bytes.c[10] == 0xff && bytes.c[11] == 0xff;
    return !mapped;
}

std::optional<quint16> parsePort(QStringView text)
{
    if (text.isEmpty() || text.size() > 5)
        return {};
    for (QChar c : text)
        if (c < u'0' || c > u'9')
            return {};
    bool ok = false;
    const auto port = text.toUShort(&ok);
    if (!ok || port == 0)
        return {};
    return port;
}

} // namespace

QList<LanAddress> systemLanAddresses()
{
    QList<LanAddress> result;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack) || flags.testFlag(QNetworkInterface::IsPointToPoint))
            continue;
        if (iface.type() != QNetworkInterface::Ethernet && iface.type() != QNetworkInterface::Wifi)
            continue;
        for (const auto &entry : iface.addressEntries()) {
            const auto address = unscoped(entry.ip());
            if (iface.index() <= 0 || !isUnicast(address) || address.isLoopback()
                || entry.prefixLength() <= 0 || entry.prefixLength() > (isV4(address) ? 32 : 128))
                continue;
            LanAddress link{address, entry.prefixLength(), static_cast<unsigned int>(iface.index()), iface.name()};
            if (!result.contains(link))
                result.append(link);
        }
    }
    std::sort(result.begin(), result.end(), [](const LanAddress &a, const LanAddress &b) {
        if (isV4(a.address) != isV4(b.address))
            return isV4(a.address);
        if (a.interfaceIndex != b.interfaceIndex)
            return a.interfaceIndex < b.interfaceIndex;
        return a.address.toString() < b.address.toString();
    });
    return result;
}

std::optional<Endpoint> parseEndpoint(QString text)
{
    text = text.trimmed();
    if (text.isEmpty() || text.size() > 160)
        return {};
    QString literal = text;
    std::optional<quint16> port = defaultProbePort;
    const bool bracketed = text.startsWith(u'[');
    if (bracketed) {
        const auto close = text.indexOf(u']');
        if (close < 0)
            return {};
        literal = text.mid(1, close - 1);
        if (close + 1 < text.size()) {
            if (text[close + 1] != u':')
                return {};
            port = parsePort(QStringView(text).sliced(close + 2));
        }
    } else if (text.count(u':') == 1) {
        const auto colon = text.indexOf(u':');
        literal = text.left(colon);
        port = parsePort(QStringView(text).sliced(colon + 1));
    }
    QHostAddress address;
    if (!port || !address.setAddress(literal) || !isUnicast(address))
        return {};
    // Accept canonical dotted decimal only. QHostAddress also accepts legacy IPv4 shorthand.
    if (isV4(address) && (bracketed || address.toString() != literal))
        return {};
    return Endpoint{address, *port};
}

QString formatEndpoint(const Endpoint &endpoint)
{
    const auto host = endpoint.address.toString();
    return isV4(endpoint.address) ? QStringLiteral("%1:%2").arg(host).arg(endpoint.port)
                                  : QStringLiteral("[%1]:%2").arg(host).arg(endpoint.port);
}

bool isOnLink(const QHostAddress &peer, const LanAddress &link)
{
    const int bits = isV4(link.address) ? 32 : 128;
    if (!isUnicast(peer) || link.prefixLength <= 0 || link.prefixLength > bits
        || peer.protocol() != link.address.protocol() || peer.isLoopback() != link.address.isLoopback())
        return false;
    const auto scope = peer.scopeId();
    if (!scope.isEmpty() && scope != link.interfaceName && scope != QString::number(link.interfaceIndex))
        return false;
    if (!unscoped(peer).isInSubnet(unscoped(link.address), link.prefixLength))
        return false;
    if (isV4(peer) && link.prefixLength < 31) {
        const quint32 hostMask = std::numeric_limits<quint32>::max() >> link.prefixLength;
        const quint32 host = peer.toIPv4Address() & hostMask;
        if (host == 0 || host == hostMask)
            return false;
    }
    return true;
}

std::optional<LanAddress> selectLink(const QHostAddress &peer, const QList<LanAddress> &links)
{
    std::optional<LanAddress> best;
    bool ambiguous = false;
    for (const auto &link : links) {
        if (!isOnLink(peer, link))
            continue;
        if (!best || link.prefixLength > best->prefixLength) {
            best = link;
            ambiguous = false;
        } else if (best->prefixLength == link.prefixLength && best->interfaceIndex != link.interfaceIndex) {
            ambiguous = true;
        }
    }
    if (ambiguous)
        return {};
    return best;
}

QHostAddress socketAddress(QHostAddress address, const LanAddress &link)
{
    if (address.protocol() == QAbstractSocket::IPv6Protocol && address.isLinkLocal())
        address.setScopeId(QString::number(link.interfaceIndex));
    return address;
}

bool pinToLink(qintptr descriptor, const LanAddress &link)
{
    const bool v4 = isV4(link.address);
#ifdef Q_OS_WIN
    const auto fd = static_cast<SOCKET>(descriptor);
#else
    const auto fd = static_cast<int>(descriptor);
#endif
    const int level = v4 ? IPPROTO_IP : IPPROTO_IPV6;
    const int oneHop = 1;
    if (::setsockopt(fd, level, v4 ? IP_TTL : IPV6_UNICAST_HOPS,
                     reinterpret_cast<const char *>(&oneHop), sizeof(oneHop)) != 0)
        return false;
#ifdef Q_OS_MACOS
    const unsigned int index = link.interfaceIndex;
    const int option = v4 ? IP_BOUND_IF : IPV6_BOUND_IF;
#elif defined(Q_OS_WIN)
    // Winsock expects the IPv4 interface index in network byte order and IPv6 in host byte order.
    const unsigned int index = v4 ? htonl(link.interfaceIndex) : link.interfaceIndex;
    const int option = v4 ? IP_UNICAST_IF : IPV6_UNICAST_IF;
#else
#error A unicast interface binding is required on this platform
#endif
    return ::setsockopt(fd, level, option, reinterpret_cast<const char *>(&index), sizeof(index)) == 0;
}

bool bindLanClient(QTcpSocket &socket, const LanAddress &link)
{
    socket.setProxy(QNetworkProxy::NoProxy);
    return socket.bind(socketAddress(link.address, link), 0) && pinToLink(socket.socketDescriptor(), link);
}
} // namespace wwmm
