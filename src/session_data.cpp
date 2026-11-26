#include "session_data.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QSaveFile>
#include <QTcpServer>
#include <QUuid>
#include <QtEndian>
#include <atomic>
#include <utility>
#include <limits>

namespace wwmm {
namespace {
constexpr int chunk = 16 * 1024;

template <class T> QByteArray number(T value)
{
    QByteArray bytes(sizeof(T), Qt::Uninitialized);
    qToBigEndian(value, bytes.data());
    return bytes;
}

template <class T> T field(const QByteArray &bytes, int offset)
{
    return qFromBigEndian<T>(bytes.constData() + offset);
}

QString safeName(QString name)
{
    name.replace(u'\\', u'/');
    name = name.section(u'/', -1);
    for (auto &c : name)
        if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format || c == u':')
            c = u'_';
    if (name.isEmpty() || name == "." || name == "..")
        return QStringLiteral("document");
    return name;
}
} // namespace

struct SessionData::Disk {
    std::unique_ptr<QFile> source;
    std::unique_ptr<QSaveFile> target;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    std::atomic_bool cancelled{false};
};

SessionData::SessionData(DeviceStore &store, QObject *parent) : QObject(parent), store_(store)
{
    pump_.setSingleShot(true);
    pump_.setTimerType(Qt::PreciseTimer);
    pump_.setInterval(8);
    connect(&pump_, &QTimer::timeout, this, &SessionData::tick);
    bindTimer_.setSingleShot(true);
    bindTimer_.setInterval(8000);
    connect(&bindTimer_, &QTimer::timeout, this, &SessionData::failed);
}

SessionData::~SessionData()
{
    reset();
    if (diskWorker_) {
        diskThread_.quit();
        diskThread_.wait();
    }
}

QByteArray SessionData::listen(const LanAddress &link, QHostAddress peer, QByteArray key, bool rich)
{
    reset();
    rich_ = rich;
    link_ = link;
    remote_ = std::move(peer);
    peer_ = std::move(key);
    host_ = true;
    server_ = new QTcpServer(this);
    server_->setProxy(QNetworkProxy::NoProxy);
    server_->setMaxPendingConnections(1);
    if (!server_->listen(socketAddress(link.address, link), 0) ||
        !pinToLink(server_->socketDescriptor(), link)) {
        failed();
        return {};
    }
    token_ = QUuid::createUuid().toRfc4122();
    connect(server_, &QTcpServer::newConnection, this, [this] {
        while (server_ && server_->hasPendingConnections()) {
            auto *socket = server_->nextPendingConnection();
            if (channel_ || socket->peerAddress() != remote_ || !isOnLink(socket->peerAddress(), link_) ||
                !pinToLink(socket->socketDescriptor(), link_)) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            attach(socket, true);
            channel_->start();
        }
    });
    bindTimer_.start();
    return QByteArray("B") + number(server_->serverPort()) + token_;
}

void SessionData::join(const LanAddress &link, QHostAddress peer, QByteArray key, const QByteArray &offer,
                       bool rich)
{
    if (offer.size() != 19 || offer[0] != 'B' || !isOnLink(peer, link) || !field<quint16>(offer, 1) ||
        channel_ || bound_)
        return;
    reset();
    rich_ = rich;
    peer_ = std::move(key);
    remote_ = std::move(peer);
    link_ = link;
    token_ = offer.mid(3);
    auto *socket = new QTcpSocket(this);
    if (!bindLanClient(*socket, link)) {
        socket->deleteLater();
        return failed();
    }
    attach(socket, false);
    connect(socket, &QTcpSocket::connected, channel_, &TrustedChannel::start);
    bindTimer_.start();
    socket->connectToHost(socketAddress(remote_, link), field<quint16>(offer, 1));
}

void SessionData::attach(QTcpSocket *socket, bool server)
{
    channel_ =
        new TrustedChannel(socket, store_, peer_, this,
                           server ? TrustedChannel::Mode::DataServer : TrustedChannel::Mode::DataClient);
    connect(channel_, &TrustedChannel::closed, this, &SessionData::failed);
    connect(channel_, &TrustedChannel::message, this, &SessionData::handle);
    connect(channel_, &TrustedChannel::authenticated, this, [this] {
        if (!host_)
            send("h" + token_);
    });
}

bool SessionData::send(const QByteArray &frame)
{
    if (channel_ && channel_->send(frame))
        return true;
    failed();
    return false;
}

void SessionData::failed()
{
    reset();
    emit notice(tr("剪贴板与文件连接已中断。重新连接桌面可恢复。"));
}

void SessionData::reset()
{
    pump_.stop();
    bindTimer_.stop();
    if (server_) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }
    if (channel_) {
        auto *old = std::exchange(channel_, nullptr);
        old->disconnect(this);
        old->stop({});
        old->deleteLater();
    }
    bound_ = host_ = localClip_ = peerClip_ = rich_ = false;
    clearClipboard();
    clearFile();
    token_.clear();
    peer_.clear();
    revision_ = 0;
    writer_.clear();
    emit changed();
}

void SessionData::clearClipboard()
{
    incomingClip_.clear();
    outgoingClip_.clear();
    startClip_ = inImage_ = outImage_ = false;
    clipOffset_ = expectedClipBytes_ = 0;
    receivingClip_ = pendingClip_ = 0;
    emit clipboardSettled();
}

void SessionData::enableClipboard(bool enable)
{
    if (!bound_ || enable == localClip_)
        return;
    localClip_ = enable;
    if (!enable)
        clearClipboard();
    if (send(enable ? "E" : "e"))
        emit changed();
}

void SessionData::copyText(const QString &text)
{
    if (!clipboardEnabled())
        return;
    auto bytes = text.size() <= maxTextBytes ? text.toUtf8() : QByteArray{};
    if (text.size() > maxTextBytes || bytes.size() > maxTextBytes) {
        discardLocalCopy();
        emit notice(tr("文字超过 1 MiB，可保存成文件发送。"));
        return;
    }
    if (!beginCopy(false))
        return;
    outgoingClip_ = std::move(bytes);
    startClip_ = true;
    tick();
}

quint64 SessionData::beginCopy(bool image)
{
    if (!clipboardEnabled())
        return 0;
    if (pendingClip_ && rich_ && !send("w" + number(pendingClip_)))
        return 0;
    if (receivingClip_ && rich_ && !send("x" + number(receivingClip_)))
        return 0;
    writer_ = store_.publicKey();
    receivingClip_ = 0;
    incomingClip_.clear();
    outgoingClip_.clear();
    startClip_ = false;
    outImage_ = image;
    inImage_ = false;
    clipOffset_ = 0;
    pendingClip_ = ++revision_;
    return pendingClip_;
}

void SessionData::discardLocalCopy()
{
    if (pendingClip_ && rich_)
        send("w" + number(pendingClip_));
    if (receivingClip_ && rich_)
        send("x" + number(receivingClip_));
    emit clipboardRejected();
    clearClipboard();
}

quint64 SessionData::beginImage()
{
    const auto rev = rich_ ? beginCopy(true) : 0;
    if (rev)
        emit changed();
    return rev;
}

void SessionData::encodedImage(quint64 rev, QByteArray png)
{
    if (!rev || rev != pendingClip_ || !outImage_ || !clipboardEnabled())
        return;
    if (png.isEmpty() || png.size() > maxImageBytes) {
        discardLocalCopy();
        emit notice(tr("截图超出限制或无法编码，可作为文件发送。"));
        return;
    }
    outgoingClip_ = std::move(png);
    startClip_ = true;
    tick();
}

void SessionData::finishImage(quint64 rev, bool applied)
{
    if (!expectsImage(rev))
        return;
    receivingClip_ = 0;
    if (!applied)
        emit clipboardRejected();
    send((applied ? QByteArray("k") : QByteArray("x")) + number(rev));
    emit clipboardSettled();
    if (!applied)
        emit notice(tr("未能将收到的截图写入剪贴板。"));
}

void SessionData::handle(const QByteArray &f)
{
    if (!bound_) {
        if (f != "h" + token_)
            return failed();
        if (host_ && !send(f))
            return;
        bound_ = true;
        bindTimer_.stop();
        if (server_)
            server_->close();
        token_.clear();
        emit changed();
        return;
    }
    if (f == "E" || f == "e") {
        peerClip_ = f == "E";
        if (!peerClip_)
            clearClipboard();
        emit changed();
        return;
    }
    const char op = f[0];
    if (op == 'b' || op == 't' || op == 'k' || (rich_ && (op == 'g' || op == 'x' || op == 'w')))
        return handleClipboard(f);
    handleFile(f);
}

void SessionData::handleClipboard(const QByteArray &f)
{
    if (!clipboardEnabled())
        return;
    if (f.size() < 9)
        return failed();
    const char op = f[0];
    const auto rev = field<quint64>(f, 1);
    if (op == 'k' || op == 'x' || op == 'w') {
        if (f.size() != 9 || !rev)
            return failed();
        if (op == 'w') {
            if (receivingClip_ != rev)
                return;
            receivingClip_ = 0;
            incomingClip_.clear();
            emit clipboardRejected();
        } else {
            if (pendingClip_ != rev)
                return;
            pendingClip_ = 0;
            outgoingClip_.clear();
            startClip_ = false;
            if (op == 'x')
                emit clipboardRejected();
        }
        emit clipboardSettled();
        return;
    }
    if (op == 'b' || op == 'g') {
        const bool image = op == 'g';
        if (f.size() != 13 || !rev || rev == std::numeric_limits<quint64>::max())
            return failed();
        const auto size = field<quint32>(f, 9);
        if (size > quint32(image ? maxImageBytes : maxTextBytes) || (image && !size))
            return failed();
        if (rev < revision_ || (rev == revision_ && peer_ <= writer_))
            return;
        if (pendingClip_)
            emit clipboardRejected();
        pendingClip_ = 0;
        startClip_ = false;
        outgoingClip_.clear();
        writer_ = peer_;
        revision_ = receivingClip_ = rev;
        inImage_ = image;
        incomingClip_.clear();
        expectedClipBytes_ = int(size);
        incomingClip_.reserve(expectedClipBytes_);
        if (image)
            emit changed();
    } else {
        if (rev != receivingClip_ || !receivingClip_)
            return;
        if (f.size() <= 9 || f.size() - 9 > expectedClipBytes_ - incomingClip_.size())
            return failed();
        incomingClip_.append(f.constData() + 9, f.size() - 9);
    }
    if (incomingClip_.size() != expectedClipBytes_)
        return;
    auto content = std::exchange(incomingClip_, {});
    expectedClipBytes_ = 0;
    if (inImage_) {
        emit clipboardImage(rev, content);
        return; // The receiver acknowledges only after decoding and installing the image.
    }
    const auto value = QString::fromUtf8(content);
    if (value.toUtf8() != content)
        return failed();
    receivingClip_ = 0;
    emit clipboardText(value);
    send("k" + number(rev));
    emit clipboardSettled();
}

void SessionData::disk(Operation work, std::function<void(DiskResult)> finish)
{
    if (!diskWorker_) {
        diskWorker_ = new QObject;
        diskWorker_->moveToThread(&diskThread_);
        connect(&diskThread_, &QThread::finished, diskWorker_, &QObject::deleteLater);
        diskThread_.start(QThread::LowPriority);
    }
    auto job = file_;
    diskBusy_ = true;
    QMetaObject::invokeMethod(
        diskWorker_,
        [this, job, work = std::move(work), finish = std::move(finish)] {
            if (job->cancelled)
                return;
            auto result = work(*job);
            QMetaObject::invokeMethod(
                this,
                [this, job, result = std::move(result), finish]() mutable {
                    if (job != file_ || job->cancelled)
                        return;
                    diskBusy_ = false;
                    finish(std::move(result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void SessionData::clearFile()
{
    if (file_) {
        file_->cancelled = true;
        auto old = std::exchange(file_, {});
        if (diskWorker_)
            QMetaObject::invokeMethod(
                diskWorker_,
                [old] {
                    old->source.reset();
                    old->target.reset();
                },
                Qt::QueuedConnection);
    }
    fileState_ = NoFile;
    id_.clear();
    name_.clear();
    total_ = progress_ = awaiting_ = 0;
    diskBusy_ = ending_ = false;
}

void SessionData::cancelFile()
{
    if (fileState_ == NoFile)
        return;
    auto id = id_;
    clearFile();
    if (bound_)
        send("X" + id);
    emit changed();
}

void SessionData::fileError(const QString &reason)
{
    cancelFile();
    emit notice(reason);
}

void SessionData::offerFile(const QString &path)
{
    if (!bound_ || fileState_ != NoFile || path.isEmpty())
        return;
    file_ = std::make_shared<Disk>();
    fileState_ = Offering;
    id_ = QUuid::createUuid().toRfc4122();
    name_ = safeName(QFileInfo(path).fileName());
    emit changed();
    disk(
        [path](Disk &d) -> DiskResult {
            d.source = std::make_unique<QFile>(path);
            const bool ok =
                QFileInfo(path).isFile() && d.source->open(QIODevice::ReadOnly) && !d.source->isSequential();
            return {ok, {}, ok ? d.source->size() : -1};
        },
        [this](DiskResult r) {
            if (!r.ok || r.size < 0)
                return fileError(tr("无法读取所选文件。"));
            total_ = r.size;
            send("O" + id_ + number(total_) + name_.toUtf8());
            emit changed();
        });
}

void SessionData::acceptFile(const QString &path)
{
    if (fileState_ != Offered || diskBusy_ || path.isEmpty())
        return;
    disk(
        [path](Disk &d) -> DiskResult {
            d.target = std::make_unique<QSaveFile>(path);
            d.target->setDirectWriteFallback(false);
            return {d.target->open(QIODevice::WriteOnly), {}};
        },
        [this](DiskResult r) {
            if (!r.ok)
                return fileError(tr("无法在所选位置保存文件。"));
            fileState_ = Receiving;
            send("A" + id_);
            emit changed();
        });
}

void SessionData::handleFile(const QByteArray &f)
{
    if (f.size() < 17)
        return failed();
    const auto id = f.mid(1, 16);
    const char op = f[0];
    if (op == 'O') {
        if (f.size() < 26 || f.size() > 1049 || field<qint64>(f, 17) < 0)
            return failed();
        if (fileState_ != NoFile) {
            send("X" + id);
            return;
        }
        id_ = id;
        name_ = safeName(QString::fromUtf8(f.mid(25)));
        total_ = field<qint64>(f, 17);
        file_ = std::make_shared<Disk>();
        fileState_ = Offered;
        emit changed();
        return;
    }
    if (id != id_ || fileState_ == NoFile)
        return; // Previously cancelled transfer; already queued records can still arrive.
    if (op == 'X') {
        clearFile();
        emit changed();
        emit notice(tr("对方已取消文件传输。"));
    } else if (op == 'A' && f.size() == 17 && fileState_ == Offering && !diskBusy_) {
        fileState_ = Sending;
        emit changed();
        pump_.start();
    } else if (op == 'P' && f.size() == 25 && fileState_ == Sending && awaiting_ > progress_ &&
               field<qint64>(f, 17) == awaiting_) {
        progress_ = awaiting_;
        emit progressChanged();
        pump_.start();
    } else if (op == 'D' && fileState_ == Receiving && !diskBusy_ && f.size() > 17 &&
               f.size() - 17 <= qMin<qint64>(chunk, total_ - progress_)) {
        disk(
            [data = f.mid(17)](Disk &d) -> DiskResult {
                const bool ok = d.target->write(data) == data.size();
                d.hash.addData(data);
                return {ok, {}, data.size()};
            },
            [this](DiskResult r) {
                if (!r.ok)
                    return fileError(tr("写入文件失败。"));
                progress_ += r.size;
                send("P" + id_ + number(progress_));
                emit progressChanged();
            });
    } else if (op == 'Z' && f.size() == 49 && fileState_ == Receiving && !diskBusy_ && progress_ == total_) {
        disk(
            [digest = f.mid(17)](Disk &d) -> DiskResult {
                return {d.hash.result() == digest && !d.cancelled && d.target->commit(), {}};
            },
            [this](DiskResult r) {
                if (!r.ok)
                    return fileError(tr("文件校验或保存失败。"));
                const auto message = tr("已校验并保存 %1。").arg(name_);
                send("H" + id_);
                clearFile();
                emit changed();
                emit notice(message);
            });
    } else if (op == 'H' && f.size() == 17 && fileState_ == Sending && ending_) {
        const auto message = tr("对方已校验并保存 %1。").arg(name_);
        clearFile();
        emit changed();
        emit notice(message);
    } else {
        fileError(tr("文件传输状态无效，已取消。"));
    }
}

void SessionData::tick()
{
    if (!bound_)
        return;
    if (channel_->pendingBytes() > chunk) {
        pump_.start();
        return;
    }
    if (startClip_) {
        startClip_ = false;
        if (!send((outImage_ ? QByteArray("g") : QByteArray("b")) + number(pendingClip_) +
                  number(quint32(outgoingClip_.size()))))
            return;
    }
    if (clipOffset_ < outgoingClip_.size()) {
        const auto part = outgoingClip_.mid(clipOffset_, chunk);
        if (!send("t" + number(pendingClip_) + part))
            return;
        clipOffset_ += int(part.size());
        if (clipOffset_ == outgoingClip_.size())
            outgoingClip_.clear();
        pump_.start();
        return;
    }
    if (fileState_ != Sending || diskBusy_ || ending_ || awaiting_ != progress_)
        return;
    const bool busy = activity_.isValid() && activity_.elapsed() < 400;
    const auto size = qMin<qint64>(busy ? chunk / 4 : chunk, total_ - progress_);
    disk(
        [size](Disk &d) -> DiskResult {
            if (!size)
                return {d.source->atEnd(), d.hash.result()};
            auto data = d.source->read(size);
            if (data.size() != size)
                return {false, {}};
            d.hash.addData(data);
            return {true, std::move(data)};
        },
        [this, size](DiskResult r) {
            if (!r.ok)
                return fileError(tr("源文件已改变或无法继续读取。"));
            if (!size) {
                ending_ = true;
                send("Z" + id_ + r.data);
            } else {
                awaiting_ = progress_ + r.data.size();
                send("D" + id_ + r.data);
            }
        });
}
} // namespace wwmm
