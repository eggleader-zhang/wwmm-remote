#include "device_store.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSysInfo>
#include <QtConcurrentRun>
#include <memory>
#include <openssl/curve25519.h>
#include <openssl/mem.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

namespace wwmm
{
namespace
{
auto bytes(const QByteArray &a) { return reinterpret_cast<const uint8_t *>(a.constData()); }
auto writable(QByteArray &a) { return reinterpret_cast<uint8_t *>(a.data()); }
struct OpenedStore {
    QByteArray publicKey, signingKey;
    QMap<QByteArray, QString> peers;
    QString error;
    ~OpenedStore() { OPENSSL_cleanse(signingKey.data(), signingKey.size()); }
};
} // namespace

bool validDeviceName(const QString &name)
{
    if (name.trimmed().isEmpty() || name.toUtf8().size() > 63)
        return false;
    for (auto cp : name.toUcs4())
        if (!QChar::isPrint(cp))
            return false;
    return true;
}

QString localDeviceName()
{
    QString name = QSysInfo::machineHostName().trimmed();
    while (name.toUtf8().size() > 63)
        name.chop(1);
    return validDeviceName(name) ? name : QStringLiteral("WWMM Remote");
}

QString fingerprint(const QByteArray &key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex().left(12));
}

DeviceStore::DeviceStore(QString directory, VaultIO vault)
    : directory_(std::move(directory)), vault_(std::move(vault))
{
}

DeviceStore::~DeviceStore() { OPENSSL_cleanse(signingKey_.data(), signingKey_.size()); }

bool DeviceStore::open(QString &error) { return update({}, {}, false, error); }

bool DeviceStore::addTlsIdentity(SSL_CTX *context) const
{
    if (signingKey_.size() != 64)
        return false;
    bssl::UniquePtr<EVP_PKEY> key(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, bytes(signingKey_), 32));
    bssl::UniquePtr<SSL_CREDENTIAL> credential(key ? SSL_CREDENTIAL_new_raw_public_key(key.get()) : nullptr);
    return credential && SSL_CTX_add1_credential(context, credential.get());
}

void DeviceStore::openAsync()
{
    if (opening_)
        return;
    opening_ = true;
    auto *job = new QFutureWatcher<std::shared_ptr<OpenedStore>>(this);
    connect(job, &QFutureWatcherBase::finished, this, [this, job, revision = revision_] {
        const auto snapshot = job->result();
        job->deleteLater();
        opening_ = false;
        // A local pairing or revocation may have committed while this result was queued.
        if (snapshot->error.isEmpty() && revision == revision_) {
            if (publicKey_.isEmpty()) {
                publicKey_.swap(snapshot->publicKey);
                signingKey_.swap(snapshot->signingKey);
            }
            if (peers_ != snapshot->peers) {
                peers_.swap(snapshot->peers);
                emit changed();
            }
        }
        emit openFinished(snapshot->error);
    });
    job->setFuture(QtConcurrent::run([dir = directory_, vault = vault_, expectedKey = publicKey_] {
        auto snapshot = std::make_shared<OpenedStore>();
        DeviceStore reader(dir, vault);
        reader.publicKey_ = expectedKey;
        if (reader.open(snapshot->error)) {
            snapshot->publicKey.swap(reader.publicKey_);
            snapshot->signingKey.swap(reader.signingKey_);
            snapshot->peers.swap(reader.peers_);
        } else if (snapshot->error.isEmpty()) {
            snapshot->error = tr("无法读取本机设备身份。");
        }
        return snapshot;
    }));
}

bool DeviceStore::remember(const QByteArray &key, const QString &name, QString &error)
{
    if (key.size() != 32 || key == publicKey_ || !validDeviceName(name)) {
        error = tr("设备身份无效。");
        return false;
    }
    return update(key, name, false, error);
}
bool DeviceStore::forget(const QByteArray &key, QString &error)
{
    if (key.size() != 32) {
        error = tr("设备身份无效。");
        return false;
    }
    return update(key, {}, true, error);
}

bool DeviceStore::update(const QByteArray &key, const QString &name, bool remove, QString &error)
{
    if (!QDir().mkpath(directory_)) {
        error = tr("无法创建本机设备数据目录。");
        return false;
    }
    QLockFile lock(QDir(directory_).filePath("identity.lock"));
    // The OS may wait indefinitely for the user to unlock the identity.
    lock.setStaleLockTime(0);
    if (!lock.tryLock(400)) {
        error = tr("设备数据正在使用，请重试。");
        return false;
    }
    QByteArray record;
    if (!vault_.read(record, error))
        return false;
    const bool create = record.isEmpty() && publicKey_.isEmpty() && key.isEmpty();
    auto obj = QJsonDocument::fromJson(record).object();
    QByteArray priv(64, '\0'), pub(32, '\0');
    struct Wipe {
        QByteArray &value;
        ~Wipe() { OPENSSL_cleanse(value.data(), value.size()); }
    } wipe{priv};
    if (create) {
        ED25519_keypair(writable(pub), writable(priv));
        obj.insert("version", 1);
        obj.insert("seed", QString::fromLatin1(priv.first(32).toBase64()));
        obj.insert("peers", QJsonObject{});
    } else {
        auto seed = QByteArray::fromBase64(obj["seed"].toString().toLatin1(),
                                           QByteArray::AbortOnBase64DecodingErrors);
        Wipe wipeSeed{seed};
        if (seed.size() != 32 || obj["version"].toInt() != 1 || !obj["peers"].isObject()) {
            error = tr("无法读取原有设备身份。");
            return false;
        }
        ED25519_keypair_from_seed(writable(pub), writable(priv), bytes(seed));
    }
    if (!publicKey_.isEmpty() && pub != publicKey_) {
        error = tr("本机设备身份已改变，请重新打开应用。");
        return false;
    }
    auto peers = obj["peers"].toObject();
    QMap<QByteArray, QString> verified;
    for (auto it = peers.begin(); it != peers.end(); ++it) {
        const auto raw = QByteArray::fromHex(it.key().toLatin1());
        if (raw.size() != 32 || raw.toHex() != it.key().toLatin1() || raw == pub ||
            !validDeviceName(it.value().toString())) {
            error = tr("已配对设备记录损坏。");
            return false;
        }
        verified.insert(raw, it.value().toString());
    }
    if (!key.isEmpty()) {
        const auto id = QString::fromLatin1(key.toHex());
        if (remove) {
            peers.remove(id);
            verified.remove(key);
        } else {
            if (!verified.contains(key) && verified.size() >= 128) {
                error = tr("已保存 128 台设备，请先撤销不用的设备。");
                return false;
            }
            peers.insert(id, name);
            verified.insert(key, name);
        }
        obj.insert("peers", peers);
    }
    if ((create || !key.isEmpty()) && !vault_.write(QJsonDocument(obj).toJson(QJsonDocument::Compact), error))
        return false;
    if (publicKey_.isEmpty()) {
        publicKey_ = pub;
        signingKey_ = QByteArray(priv.constData(), priv.size());
    }
    const bool notify = peers_ != verified;
    peers_ = std::move(verified);
    ++revision_;
    lock.unlock();
    if (notify)
        emit changed();
    return true;
}

QByteArray DeviceStore::sign(const QByteArray &message) const
{
    QByteArray signature(64, '\0');
    return signingKey_.size() == 64 &&
                   ED25519_sign(writable(signature), bytes(message), message.size(), bytes(signingKey_))
               ? signature
               : QByteArray{};
}

bool DeviceStore::verify(const QByteArray &key, const QByteArray &message, const QByteArray &signature)
{
    return key.size() == 32 && signature.size() == 64 &&
           ED25519_verify(bytes(message), message.size(), bytes(signature), bytes(key)) == 1;
}
} // namespace wwmm
