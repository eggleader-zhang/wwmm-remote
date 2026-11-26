#include "trusted_channel.h"
#include <QTimer>
#include <QtEndian>
#include <openssl/err.h>
#include <openssl/evp.h>

namespace wwmm
{
TrustedChannel::TrustedChannel(QTcpSocket *socket, DeviceStore &store, QByteArray expected, QObject *parent,
                               Mode mode)
    : QObject(parent), socket_(socket), store_(store), expected_(std::move(expected)), mode_(mode)
{
    socket->setParent(this);
    socket->setReadBufferSize(32768);
    connect(socket, &QTcpSocket::readyRead, this, &TrustedChannel::pump);
    connect(socket, &QTcpSocket::disconnected, this, [this] { stop(tr("对方已结束桌面会话。")); });
    connect(socket, &QTcpSocket::errorOccurred, this, [this] { stop(tr("桌面连接已断开。")); });
    QTimer::singleShot(7000, this, [this] {
        if (!ready_)
            stop(tr("设备身份验证超时。"));
    });
}

ssl_verify_result_t TrustedChannel::verify(SSL *ssl, uint8_t *alert)
{
    auto *self = static_cast<TrustedChannel *>(SSL_get_app_data(ssl));
    EVP_PKEY *key = SSL_get0_peer_rpk(ssl);
    QByteArray raw(32, '\0');
    size_t size = 32;
    if (!self || !key || EVP_PKEY_id(key) != EVP_PKEY_ED25519 ||
        !EVP_PKEY_get_raw_public_key(key, reinterpret_cast<uint8_t *>(raw.data()), &size) || size != 32 ||
        !self->store_.peers().contains(raw) || (!self->expected_.isEmpty() && raw != self->expected_)) {
        *alert = SSL_AD_ACCESS_DENIED;
        return ssl_verify_invalid;
    }
    self->peer_ = raw;
    return ssl_verify_ok;
}

void TrustedChannel::start()
{
    if (ended_ || tls_)
        return;
    bssl::UniquePtr<SSL_CTX> ctx(SSL_CTX_new(TLS_method()));
    constexpr uint8_t rawKey = TLSEXT_cert_type_rpk;
    constexpr uint16_t signature = SSL_SIGN_ED25519;
    if (!ctx || !SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION) ||
        !SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) ||
        !SSL_CTX_set1_accepted_peer_cert_types(ctx.get(), &rawKey, 1) ||
        !SSL_CTX_set_signing_algorithm_prefs(ctx.get(), &signature, 1) ||
        !SSL_CTX_set_verify_algorithm_prefs(ctx.get(), &signature, 1) || !store_.addTlsIdentity(ctx.get()))
        return stop(tr("无法初始化本机桌面身份。"));
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_OFF);
    SSL_CTX_set_custom_verify(ctx.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify);
    tls_.reset(SSL_new(ctx.get()));
    bssl::UniquePtr<BIO> in(BIO_new(BIO_s_mem())), out(BIO_new(BIO_s_mem()));
    if (!tls_ || !in || !out)
        return stop(tr("无法创建加密桌面连接。"));
    BIO_set_mem_eof_return(in.get(), -1);
    SSL_set_bio(tls_.get(), in.release(), out.release());
    SSL_set_app_data(tls_.get(), this);
    socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    if (mode_ == Mode::DataServer || (mode_ == Mode::Control && expected_.isEmpty()))
        SSL_set_accept_state(tls_.get());
    else {
        SSL_set_connect_state(tls_.get());
        if (mode_ == Mode::Control && socket_->write("WMV3", 4) != 4)
            return stop(tr("无法发送桌面请求。"));
    }
    pump();
}

bool TrustedChannel::flush()
{
    auto *out = SSL_get_wbio(tls_.get());
    char block[4096];
    while (BIO_pending(out)) {
        int n = BIO_read(out, block, sizeof(block));
        if (n <= 0 || socket_->bytesToWrite() > (mode_ == Mode::Control ? 32768 : 98304) ||
            socket_->write(block, n) != n)
            return false;
    }
    return true;
}

void TrustedChannel::pump()
{
    if (!tls_ || ended_)
        return;
    auto wire = socket_->readAll();
    if (!ready_)
        handshakeBytes_ += int(wire.size());
    if (handshakeBytes_ > 65536 || (!wire.isEmpty() && BIO_write(SSL_get_rbio(tls_.get()), wire.data(),
                                                                 int(wire.size())) != wire.size()))
        return stop(tr("桌面协议数据无效。"));
    if (!ready_) {
        ERR_clear_error();
        int ret = SSL_do_handshake(tls_.get());
        auto err = SSL_get_error(tls_.get(), ret);
        if (!flush())
            return stop(tr("桌面连接中断。"));
        if (ret != 1) {
            if (err != SSL_ERROR_WANT_READ)
                stop(tr("双方设备身份验证失败，请检查配对记录。"));
            return;
        }
        if (peer_.isEmpty() || !store_.peers().contains(peer_))
            return stop(tr("设备信任已撤销。"));
        ready_ = true;
        emit authenticated();
    }
    char buf[4096];
    const int prefix = mode_ == Mode::Control ? 1 : 2;
    while (!ended_) {
        ERR_clear_error();
        int n = SSL_read(tls_.get(), buf, sizeof(buf));
        if (n <= 0) {
            if (SSL_get_error(tls_.get(), n) != SSL_ERROR_WANT_READ)
                stop(tr("桌面连接已关闭。"));
            return;
        }
        clear_.append(buf, n);
        while (clear_.size() >= prefix && !ended_) {
            const int len = prefix == 1 ? quint8(clear_[0]) : qFromBigEndian<quint16>(clear_.constData());
            if (len == 0 || len > maxDataFrame)
                return stop(tr("桌面协议消息无效。"));
            if (clear_.size() < prefix + len)
                break;
            const auto frame = clear_.mid(prefix, len);
            clear_.remove(0, prefix + len);
            emit message(frame);
        }
    }
}

bool TrustedChannel::send(const QByteArray &message)
{
    if (!ready_ || ended_ || message.isEmpty() ||
        message.size() > (mode_ == Mode::Control ? 255 : maxDataFrame))
        return false;
    QByteArray data(mode_ == Mode::Control ? 1 : 2, char(message.size()));
    if (mode_ != Mode::Control)
        qToBigEndian(quint16(message.size()), data.data());
    data += message;
    if (SSL_write(tls_.get(), data.constData(), int(data.size())) != data.size() || !flush())
        return false;
    // Hand the record to the TCP stack in this event, including when the GUI is busy painting.
    socket_->flush();
    return !ended_;
}

QByteArray TrustedChannel::mediaKeys() const
{
    constexpr char label[] = "EXPORTER-WWMM-desktop-v3-SRTP";
    QByteArray keys(60, '\0');
    if (!ready_ || ended_ ||
        !SSL_export_keying_material(tls_.get(), reinterpret_cast<uint8_t *>(keys.data()), keys.size(), label,
                                    sizeof(label) - 1, nullptr, 0, 0))
        return {};
    return keys;
}

void TrustedChannel::finish(const QByteArray &lastMessage)
{
    if (ended_ || !send(lastMessage)) {
        deleteLater();
        return;
    }
    ended_ = true;
    socket_->disconnect(this);
    SSL_shutdown(tls_.get());
    flush();
    connect(socket_, &QTcpSocket::disconnected, this, &QObject::deleteLater);
    socket_->disconnectFromHost();
    QTimer::singleShot(1000, this, &QObject::deleteLater);
}

void TrustedChannel::stop(const QString &reason)
{
    if (ended_)
        return;
    ended_ = true;
    socket_->disconnect(this);
    socket_->abort();
    emit closed(reason);
}
} // namespace wwmm
