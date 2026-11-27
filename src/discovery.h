#pragma once

#include "lan_probe.h"
#include <QMap>
#include <optional>

namespace wwmm
{
using ServiceFields = QMap<QString, QString>;
struct Advertisement {
    QString instance;
    LocalEndpoint local;
    ServiceFields fields;
};
enum class DesktopAccess { Incompatible, Unspecified, ViewOnly, Control };
struct NearbyDevice {
    QByteArray key;
    QString name;
    DesktopOS os;
    Endpoint endpoint;
    LanAddress link;
    DesktopAccess desktop = DesktopAccess::Unspecified;
    bool extendedDesktop = false;
    bool richMedia = false;
    bool adaptiveVideo = false;
    bool macKeyboard = false;
    // Missing DNS-SD hints still allow an explicit, authenticated session request.
    bool canRequestDesktop(bool withInput) const
    {
        return desktop != DesktopAccess::Incompatible && (!withInput || desktop != DesktopAccess::ViewOnly);
    }
    bool operator==(const NearbyDevice &) const = default;
};

class DnsSdBackend : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void start(const QList<unsigned> &interfaces, const QList<Advertisement> &ads) = 0;
signals:
    void resolved(unsigned index, const QString &instance, quint16 port, const wwmm::ServiceFields &fields);
    void removed(unsigned index, const QString &instance);
    void problem(const QString &message);
};
DnsSdBackend *nativeDnsSd(QObject *parent);

class Discovery final : public QObject
{
    Q_OBJECT
public:
    using Factory = std::function<DnsSdBackend *(QObject *)>;
    Discovery(LanProbe &probe, QByteArray publicKey, QString name, Factory factory = nativeDnsSd);
    QList<NearbyDevice> devices() const;
    const QMap<QByteArray, NearbyDevice> &devicesByKey() const;
    QString issue() const
    {
        return issue_;
    }
    void refresh();
    void restart();
  signals:
    void changed();

private:
    void refreshOn(const QList<LanAddress> &links);
    void resolved(unsigned index, const QString &instance, quint16 port, const ServiceFields &fields);
    LanProbe &probe_;
    QByteArray key_;
    QString name_;
    Factory factory_;
    DnsSdBackend *backend_ = nullptr;
    QList<LanAddress> links_;
    QList<LocalEndpoint> listening_;
    QMap<QPair<unsigned, QString>, NearbyDevice> seen_;
    mutable std::optional<QMap<QByteArray, NearbyDevice>> byKey_;
    QString issue_;
    QElapsedTimer started_;
};
} // namespace wwmm
