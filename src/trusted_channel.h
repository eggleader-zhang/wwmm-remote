#pragma once
#include "device_store.h"
#include <QTcpSocket>
#include <openssl/ssl.h>

namespace wwmm
{
class TrustedChannel final : public QObject
{
    Q_OBJECT
public:
    // The viewer pins the selected key. The host accepts keys in its own trust store.
    TrustedChannel(QTcpSocket *socket, DeviceStore &store, QByteArray expectedHost, QObject *parent);
    void start();
    void stop(const QString &reason);
    void finish(const QByteArray &lastMessage);
    bool send(const QByteArray &message);
    QByteArray mediaKeys() const;
    const QByteArray &peerKey() const
    {
        return peer_;
    }
signals:
    void authenticated();
    void message(const QByteArray &data);
    void closed(const QString &reason);

private:
    static ssl_verify_result_t verify(SSL *ssl, uint8_t *alert);
    bool flush();
    void pump();
    QTcpSocket *socket_;
    DeviceStore &store_;
    QByteArray expected_, peer_, clear_;
    bssl::UniquePtr<SSL> tls_;
    bool ready_ = false, ended_ = false;
    int handshakeBytes_ = 0;
};
} // namespace wwmm
