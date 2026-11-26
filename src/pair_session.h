#pragma once

#include "device_store.h"
#include <QTcpSocket>
#include <openssl/ssl.h>
#include <memory>

namespace wwmm
{
using PairContext = std::shared_ptr<SSL_CTX>;
PairContext pairingContext(const QString &code, const QByteArray &serverKey, bool server);

class PairSession final : public QObject
{
    Q_OBJECT
public:
    using Save = std::function<bool(const QByteArray &, const QString &, QString &)>;
    PairSession(QTcpSocket *socket, const PairContext &ctx, DeviceStore &store, QString localName,
                QByteArray expectedServer, Save save, QObject *parent);
    void start();
    void stop(const QString &reason);
signals:
    void finished(bool ok, const QString &message);

private:
    void pump();
    bool flush();
    bool sendFrame(const QByteArray &data);
    bool receiveFrame(const QByteArray &data);
    QByteArray signedData(char role, const QByteArray &client, const QByteArray &server,
                          const QByteArray &name) const;
    QByteArray proof(char role) const;
    bool readProof(const QByteArray &frame, char role);
    void end(bool success, const QString &reason);
    QTcpSocket *socket_;
    bssl::UniquePtr<SSL> tls_;
    DeviceStore &store_;
    QString localName_;
    QByteArray expected_, binding_, peerKey_, plaintext_;
    QString peerName_;
    Save save_;
    char next_;
    int bytesIn_ = 0;
    bool secured_ = false, ended_ = false;
};
} // namespace wwmm
