#include "probe_message.h"

namespace wwmm {
namespace {
const QByteArray requestPrefix("WWMM\x01\x01\0\0", 8);
const QByteArray replyPrefix("WWMM\x01\x02", 6);
}

DesktopOS localOS()
{
#ifdef Q_OS_WIN
    return DesktopOS::windows;
#elif defined(Q_OS_MACOS)
    return DesktopOS::macOS;
#else
#error WWMM Remote supports macOS and Windows
#endif
}

QString osName(DesktopOS os)
{
    return os == DesktopOS::macOS ? QStringLiteral("macOS") : QStringLiteral("Windows");
}

QByteArray probeRequest(const QByteArray &nonce)
{
    return nonce.size() == probeNonceSize ? requestPrefix + nonce : QByteArray{};
}

QByteArray probeReply(const QByteArray &request, DesktopOS os)
{
    if (request.size() != probeMessageSize || !request.startsWith(requestPrefix))
        return {};
    QByteArray reply = request;
    reply[5] = '\x02';
    reply[6] = static_cast<char>(os);
    return reply;
}

std::optional<DesktopOS> readProbeReply(const QByteArray &reply, const QByteArray &nonce)
{
    if (nonce.size() != probeNonceSize || reply.size() != probeMessageSize
        || !reply.startsWith(replyPrefix) || reply[7] != '\0' || !reply.endsWith(nonce))
        return {};
    const auto os = static_cast<DesktopOS>(reply[6]);
    if (os != DesktopOS::macOS && os != DesktopOS::windows)
        return {};
    return os;
}

} // namespace wwmm
