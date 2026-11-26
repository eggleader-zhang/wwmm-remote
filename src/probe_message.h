#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

namespace wwmm {

enum class DesktopOS : quint8 { macOS = 1, windows = 2 };
inline constexpr int probeNonceSize = 16;
inline constexpr int probeMessageSize = 8 + probeNonceSize;

DesktopOS localOS();
QString osName(DesktopOS os);
QByteArray probeRequest(const QByteArray &nonce);
QByteArray probeReply(const QByteArray &request, DesktopOS os);
std::optional<DesktopOS> readProbeReply(const QByteArray &reply, const QByteArray &nonce);

} // namespace wwmm
