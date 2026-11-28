#include "discovery.h"
#include "device_store.h"
#include <QCryptographicHash>
#include <tuple>

namespace wwmm
{
namespace {
DesktopAccess desktopAccess(const ServiceFields &fields)
{
    const auto desktop = fields.value("desk").trimmed();
    const auto input = fields.value("input").trimmed();
    if (!desktop.isEmpty() && desktop != "3")
        return DesktopAccess::Incompatible;
    if (input == "1")
        return DesktopAccess::Control;
    if (!desktop.isEmpty() || !input.isEmpty())
        return DesktopAccess::ViewOnly;
    return DesktopAccess::Unspecified;
}
} // namespace

Discovery::Discovery(LanProbe &probe, QByteArray key, QString name, Factory factory)
    : probe_(probe), key_(std::move(key)), name_(std::move(name)), factory_(std::move(factory))
{
    connect(&probe_, &LanProbe::networkRefreshed, this, &Discovery::refreshOn);
    refresh();
}

void Discovery::restart()
{
    if (backend_) {
        backend_->disconnect(this);
        delete backend_;
        backend_ = nullptr;
    }
    refresh();
}

void Discovery::refresh()
{
    refreshOn(probe_.links());
}

void Discovery::refreshOn(const QList<LanAddress> &links)
{
    const auto listening = probe_.endpoints();
    if (key_.size() != 32 || (backend_ && links == links_ && listening == listening_ &&
                              (issue_.isEmpty() || started_.elapsed() < 30000)))
        return;
    if (backend_) {
        backend_->disconnect(this);
        backend_->deleteLater();
    }
    backend_ = factory_(this);
    started_.start();
    links_ = links;
    listening_ = listening;
    seen_.clear();
    byKey_.reset();
    issue_.clear();
    connect(backend_, &DnsSdBackend::resolved, this, &Discovery::resolved);
    connect(backend_, &DnsSdBackend::removed, this, [this](unsigned i, const QString &s) {
        if (seen_.remove({i, s.toLower()})) {
            byKey_.reset();
            emit changed();
        }
    });
    connect(backend_, &DnsSdBackend::problem, this, [this](const QString &reason) {
        issue_ = reason;
        emit changed();
    });
    QList<unsigned> indices;
    for (const auto &link : links)
        if (!indices.contains(link.interfaceIndex))
            indices.append(link.interfaceIndex);
    QList<Advertisement> ads;
    for (const auto &local : listening) {
        const auto endpoint = formatEndpoint({local.link.address, local.port});
        const auto id =
            QCryptographicHash::hash(key_ + endpoint.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
        ads.append({QStringLiteral("w-") + QString::fromLatin1(id),
                    local,
                    {{"v", "2"},
                     {"desk", "3"},
                     {"input", "1"},
                     {"office", "1"},
                     {"media", "13"},
                     {"adapt", "1"},
                     {"mackeys", "1"},
                     {"cursor", "1"},
                     {"rtt", "1"},
                     {"pk", QString::fromLatin1(key_.toHex())},
                     {"addr", local.link.address.toString()},
                     {"name", QString::fromLatin1(name_.toUtf8().toBase64(QByteArray::Base64UrlEncoding |
                                                                          QByteArray::OmitTrailingEquals))},
                     {"os", osName(localOS())}}});
    }
    backend_->start(indices, ads);
    emit changed();
}

void Discovery::resolved(unsigned index, const QString &instance, quint16 port, const ServiceFields &fields)
{
    const auto keyText = fields.value("pk").toLatin1();
    const auto key = QByteArray::fromHex(keyText);
    const auto encoded = fields.value("name").toLatin1();
    const auto utf8 = QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding |
                                                          QByteArray::AbortOnBase64DecodingErrors);
    const auto name = QString::fromUtf8(utf8);
    const auto os = fields.value("os");
    const auto literal = fields.value("addr");
    QHostAddress address;
    const auto source = qMakePair(index, instance.toLower());
    auto discard = [this, &source] {
        if (seen_.remove(source)) {
            byKey_.reset();
            emit changed();
        }
    };
    if (instance.isEmpty() || instance.size() > 255 || !port || fields.value("v") != "2" ||
        key.size() != 32 || key.toHex() != keyText || key == key_ || !validDeviceName(name) ||
        utf8.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) != encoded ||
        name.toUtf8() != utf8 || (os != "macOS" && os != "Windows") || !address.setAddress(literal) ||
        !address.scopeId().isEmpty())
        return discard();
    QList<LanAddress> matching;
    for (const auto &link : links_)
        if (link.interfaceIndex == index)
            matching.append(link);
    auto link = selectLink(address, matching);
    if (!link)
        return discard();
    NearbyDevice peer{key,
                      name,
                      os == "Windows" ? DesktopOS::windows : DesktopOS::macOS,
                      {socketAddress(address, *link), port},
                      *link,
                      desktopAccess(fields),
                      fields.value("office") == "1",
                      fields.value("media") == "13",
                      fields.value("adapt") == "1",
                      fields.value("mackeys") == "1",
                      fields.value("cursor") == "1",
                      fields.value("rtt") == "1"};
    const auto previous = seen_.constFind(source);
    if (previous == seen_.cend() ? seen_.size() >= 256 : *previous == peer)
        return;
    seen_.insert(source, peer);
    byKey_.reset();
    emit changed();
}

QList<NearbyDevice> Discovery::devices() const
{
    return devicesByKey().values();
}

const QMap<QByteArray, NearbyDevice> &Discovery::devicesByKey() const
{
    if (byKey_)
        return *byKey_;
    auto &unique = byKey_.emplace();
    auto preference = [](const NearbyDevice &d) {
        return std::tuple{d.desktop, d.extendedDesktop, d.richMedia, d.adaptiveVideo, d.cursorVisibility, d.macKeyboard,
                          d.rttProbe,
                          d.endpoint.address.protocol() == QAbstractSocket::IPv4Protocol};
    };
    for (const auto &peer : seen_) {
        auto existing = unique.constFind(peer.key);
        if (existing == unique.cend() || preference(peer) > preference(*existing))
            unique.insert(peer.key, peer);
    }
    return unique;
}
} // namespace wwmm
