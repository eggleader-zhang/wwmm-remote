#pragma once

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <functional>
#include <openssl/base.h>

namespace wwmm
{

struct VaultIO {
    // A missing item is a successful read of an empty array.
    std::function<bool(QByteArray &, QString &)> read;
    std::function<bool(const QByteArray &, QString &)> write;
};
VaultIO systemVault(const QString &directory, const QString &account = QStringLiteral("local-device"));
bool validDeviceName(const QString &name);
QString localDeviceName();
QString fingerprint(const QByteArray &publicKey);

class DeviceStore final : public QObject
{
    Q_OBJECT
  public:
    DeviceStore(QString directory, VaultIO vault);
    ~DeviceStore() override;
    bool open(QString &error);
    void openAsync();
    bool remember(const QByteArray &key, const QString &name, QString &error);
    bool forget(const QByteArray &key, QString &error);
    const QByteArray &publicKey() const { return publicKey_; }
    const QMap<QByteArray, QString> &peers() const { return peers_; }
    QByteArray sign(const QByteArray &message) const;
    bool addTlsIdentity(SSL_CTX *context) const;
    static bool verify(const QByteArray &key, const QByteArray &message, const QByteArray &signature);
  signals:
    void changed();
    void openFinished(const QString &error);

  private:
    bool update(const QByteArray &key, const QString &name, bool remove, QString &error);
    QString directory_;
    VaultIO vault_;
    QByteArray publicKey_;
    QByteArray signingKey_;
    QMap<QByteArray, QString> peers_;
    quint64 revision_ = 0;
    bool opening_ = false;
};
} // namespace wwmm
