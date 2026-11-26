#include "discovery.h"
#include "device_store.h"
#include <QCryptographicHash>
#include <QTimer>

namespace wwmm
{
Discovery::Discovery(LanProbe &probe, QByteArray key, QString name, Factory factory)
    : probe_(probe), key_(std::move(key)), name_(std::move(name)), factory_(std::move(factory))
{
    connect(&probe_, &LanProbe::endpointsChanged, this, &Discovery::refresh);
    auto *timer = new QTimer(this);
    timer->setInterval(5000);
    timer->setTimerType(Qt::VeryCoarseTimer);
    connect(timer, &QTimer::timeout, this, &Discovery::refresh);
    timer->start();
    refresh();
}

void Discovery::refresh()
{
    const auto links = probe_.links();
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
    issue_.clear();
    connect(backend_, &DnsSdBackend::resolved, this, &Discovery::resolved);
    connect(backend_, &DnsSdBackend::removed, this, [this](unsigned i, const QString &s) {
        if (seen_.remove({i, s.toLower()}))
            emit changed();
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
                    {{"v", "2"}, {"desk", "3"}, {"input", "1"},
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
        if (seen_.remove(source))
            emit changed();
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
                      *link, fields.value("desk") == "3", fields.value("input") == "1"};
    const auto previous = seen_.constFind(source);
    if (previous == seen_.cend() ? seen_.size() >= 256 : *previous == peer)
        return;
    seen_.insert(source, peer);
    emit changed();
}

QList<NearbyDevice> Discovery::devices() const
{
    QMap<QByteArray, NearbyDevice> unique;
    for (const auto &peer : seen_) {
        auto existing = unique.constFind(peer.key);
        if (existing == unique.cend() ||
            (existing->endpoint.address.protocol() == QAbstractSocket::IPv6Protocol &&
             peer.endpoint.address.protocol() == QAbstractSocket::IPv4Protocol))
            unique.insert(peer.key, peer);
    }
    return unique.values();
}
} // namespace wwmm
