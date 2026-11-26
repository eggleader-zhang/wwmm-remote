#include "pair_session.h"
#include <QTimer>
#include <QtEndian>
#include <openssl/err.h>
#include <openssl/mem.h>

namespace wwmm
{
namespace
{
auto bytes(const QByteArray &value)
{
    return reinterpret_cast<const uint8_t *>(value.constData());
}
constexpr uint8_t context[] = "WWMM Remote LAN pairing v2";
constexpr uint8_t clientId[] = "pairing-client";
constexpr char exporter[] = "EXPORTER-WWMM-pairing-v2";
const auto invalid = QT_TRANSLATE_NOOP("wwmm::PairSession", "配对验证未通过，请核对设备和配对码。");
} // namespace

PairContext pairingContext(const QString &code, const QByteArray &serverKey, bool server)
{
    if (serverKey.size() != 32 || code.size() != 8)
        return {};
    for (QChar c : code)
        if (c < u'0' || c > u'9')
            return {};
    const auto pass = code.toLatin1();
    struct Secrets {
        uint8_t w0[32], w1[32], record[65];
        ~Secrets()
        {
            OPENSSL_cleanse(this, sizeof(*this));
        }
    } secret{};
    if (!SSL_spake2plusv1_register(secret.w0, secret.w1, secret.record, bytes(pass), pass.size(), clientId,
                                   sizeof(clientId) - 1, bytes(serverKey), serverKey.size()))
        return {};
    bssl::UniquePtr<SSL_CREDENTIAL> cred(
        server ? SSL_CREDENTIAL_new_spake2plusv1_server(
                     context, sizeof(context) - 1, clientId, sizeof(clientId) - 1, bytes(serverKey),
                     serverKey.size(), 5, secret.w0, sizeof(secret.w0), secret.record, sizeof(secret.record))
               : SSL_CREDENTIAL_new_spake2plusv1_client(
                     context, sizeof(context) - 1, clientId, sizeof(clientId) - 1, bytes(serverKey),
                     serverKey.size(), 1, secret.w0, sizeof(secret.w0), secret.w1, sizeof(secret.w1)));
    PairContext ctx(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    if (!ctx || !cred || !SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION) ||
        !SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) ||
        !SSL_CTX_add1_credential(ctx.get(), cred.get()))
        return {};
    SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_OFF);
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_TICKET);
    return ctx;
}

PairSession::PairSession(QTcpSocket *socket, const PairContext &ctx, DeviceStore &store, QString name,
                         QByteArray expected, Save save, QObject *parent)
    : QObject(parent), socket_(socket), tls_(SSL_new(ctx.get())), store_(store), localName_(std::move(name)),
      expected_(std::move(expected)), save_(std::move(save)), next_(expected_.isEmpty() ? 'C' : 'S')
{
    socket_->setParent(this);
    socket_->setReadBufferSize(65536);
    connect(socket_, &QTcpSocket::readyRead, this, &PairSession::pump);
    connect(socket_, &QTcpSocket::disconnected, this, [this] {
        if (!ended_)
            stop(tr("连接已断开，请重新配对。"));
        deleteLater();
    });
    connect(socket_, &QTcpSocket::errorOccurred, this,
            [this] { stop(tr("配对连接中断，请确认对方已开启配对。")); });
    QTimer::singleShot(12000, this, [this] { stop(tr("配对连接超时。")); });
}

void PairSession::start()
{
    bssl::UniquePtr<BIO> input(BIO_new(BIO_s_mem())), output(BIO_new(BIO_s_mem()));
    if (!tls_ || !input || !output) {
        stop(tr("无法初始化加密连接。"));
        return;
    }
    BIO_set_mem_eof_return(input.get(), -1);
    SSL_set_bio(tls_.get(), input.release(), output.release());
    if (expected_.isEmpty()) {
        SSL_set_accept_state(tls_.get());
    } else {
        SSL_set_connect_state(tls_.get());
        if (socket_->write("WMP2", 4) != 4) {
            stop(tr("无法连接到对方。"));
            return;
        }
    }
    pump();
}

bool PairSession::flush()
{
    char buffer[8192];
    auto *out = SSL_get_wbio(tls_.get());
    while (BIO_pending(out)) {
        const int n = BIO_read(out, buffer, sizeof(buffer));
        if (n <= 0 || socket_->write(buffer, n) != n)
            return false;
    }
    return true;
}

void PairSession::pump()
{
    if (ended_ || !tls_ || !SSL_get_rbio(tls_.get()))
        return;
    auto wire = socket_->readAll();
    bytesIn_ += int(wire.size());
    if (bytesIn_ > 98304 || (!wire.isEmpty() && BIO_write(SSL_get_rbio(tls_.get()), wire.data(),
                                                          int(wire.size())) != wire.size())) {
        stop(tr(invalid));
        return;
    }
    if (!secured_) {
        ERR_clear_error();
        const int ret = SSL_do_handshake(tls_.get());
        const auto error = SSL_get_error(tls_.get(), ret);
        if (!flush()) {
            stop(tr(invalid));
            return;
        }
        if (ret != 1) {
            if (error != SSL_ERROR_WANT_READ)
                stop(tr(invalid));
            return;
        }
        binding_.resize(32);
        if (!SSL_export_keying_material(tls_.get(), reinterpret_cast<uint8_t *>(binding_.data()), 32,
                                        exporter, sizeof(exporter) - 1, nullptr, 0, 0)) {
            stop(tr(invalid));
            return;
        }
        secured_ = true;
        if (!expected_.isEmpty() && !sendFrame(proof('C'))) {
            stop(tr(invalid));
            return;
        }
    }
    while (!ended_) {
        char buf[512];
        ERR_clear_error();
        const int n = SSL_read(tls_.get(), buf, sizeof(buf));
        if (n <= 0) {
            if (SSL_get_error(tls_.get(), n) != SSL_ERROR_WANT_READ)
                stop(tr(invalid));
            return;
        }
        plaintext_.append(buf, n);
        while (plaintext_.size() >= 2 && !ended_) {
            const auto size = qFromBigEndian<quint16>(plaintext_.constData());
            if (!size || size > 192) {
                stop(tr(invalid));
                return;
            }
            if (plaintext_.size() < size + 2)
                break;
            const auto frame = plaintext_.mid(2, size);
            plaintext_.remove(0, size + 2);
            if (!receiveFrame(frame)) {
                stop(tr(invalid));
                return;
            }
        }
    }
}

bool PairSession::sendFrame(const QByteArray &data)
{
    if (data.isEmpty() || data.size() > 192)
        return false;
    QByteArray framed(2, '\0');
    qToBigEndian<quint16>(quint16(data.size()), framed.data());
    framed += data;
    return SSL_write(tls_.get(), framed.constData(), int(framed.size())) == framed.size() && flush();
}

QByteArray PairSession::signedData(char role, const QByteArray &client, const QByteArray &server,
                                   const QByteArray &name) const
{
    return QByteArray("WWMM pair proof v2") + role + binding_ + client + server + name;
}
QByteArray PairSession::proof(char role) const
{
    const auto &own = store_.publicKey();
    const auto name = localName_.toUtf8();
    const auto msg =
        role == 'C' ? signedData(role, own, expected_, name) : signedData(role, peerKey_, own, name);
    const auto signature = store_.sign(msg);
    return signature.size() == 64 ? QByteArray(1, role) + own + name + signature : QByteArray{};
}
bool PairSession::readProof(const QByteArray &frame, char role)
{
    if (frame.size() < 98 || frame.size() > 160)
        return false;
    const auto key = frame.mid(1, 32);
    const auto name = frame.mid(33, frame.size() - 97);
    const auto display = QString::fromUtf8(name);
    const auto &own = store_.publicKey();
    const auto msg = role == 'C' ? signedData(role, key, own, name) : signedData(role, own, key, name);
    if (key == own || (role == 'S' && key != expected_) || display.toUtf8() != name ||
        !validDeviceName(display) || !DeviceStore::verify(key, msg, frame.last(64)))
        return false;
    peerKey_ = key;
    peerName_ = display;
    return true;
}
bool PairSession::receiveFrame(const QByteArray &frame)
{
    if (frame[0] != next_)
        return false;
    if (next_ == 'C' || next_ == 'S') {
        if (!readProof(frame, next_))
            return false;
        const bool server = next_ == 'C';
        next_ = server ? 'A' : 'D';
        return sendFrame(server ? proof('S') : QByteArray("A"));
    }
    if (frame.size() != 1)
        return false;
    QString error;
    if (!save_(peerKey_, peerName_, error)) {
        stop(error);
        return false;
    }
    if (next_ == 'A' && !sendFrame("D"))
        return false;
    end(true, tr("本机已保存 %1 的设备身份。").arg(peerName_));
    return true;
}
void PairSession::stop(const QString &reason)
{
    end(false, reason);
}
void PairSession::end(bool success, const QString &reason)
{
    if (ended_)
        return;
    ended_ = true;
    emit finished(success, reason);
    if (success) {
        socket_->disconnectFromHost();
        QTimer::singleShot(2000, this, &QObject::deleteLater);
    } else {
        socket_->abort();
        deleteLater();
    }
}
} // namespace wwmm
