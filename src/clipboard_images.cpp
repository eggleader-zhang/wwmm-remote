#include "clipboard_images.h"
#include <QBuffer>
#include <QFutureWatcher>
#include <QImageReader>
#include <QImageWriter>
#include <QMimeData>
#include <QPixmap>
#include <QtConcurrentRun>
#include <utility>

namespace wwmm {
namespace {
bool allowed(QSize s)
{
    return s.width() > 0 && s.height() > 0 && s.width() <= 8192 && s.height() <= 8192 &&
           qint64(s.width()) * s.height() <= 16 * 1024 * 1024;
}

QImage readPng(QByteArray &png, bool headerOnly, bool &ok)
{
    QBuffer buffer(&png);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png");
    reader.setAutoDetectImageFormat(false);
    ok = !png.isEmpty() && png.size() <= SessionData::maxImageBytes && reader.canRead() &&
         allowed(reader.size());
    if (!ok || headerOnly)
        return {};
    return reader.read();
}
} // namespace

ClipboardImages::ClipboardImages(SessionData &data, Install install, QObject *parent)
    : QObject(parent), data_(data), install_(std::move(install))
{
    connect(&data, &SessionData::clipboardImage, this, [this](quint64 rev, const QByteArray &png) {
        nextDecode_ = Job{rev, png, {}};
        decode();
    });
    connect(&data, &SessionData::changed, this, [this] {
        if (!data_.clipboardEnabled()) {
            nextEncode_.reset();
            nextDecode_.reset();
        }
    });
}

void ClipboardImages::copy(const QMimeData &mime)
{
    const auto revision = data_.beginImage();
    if (!revision)
        return;
    Job job{revision, {}, {}};
    if (mime.hasFormat(QStringLiteral("image/png")))
        job.png = mime.data(QStringLiteral("image/png"));
    if (job.png.isEmpty()) {
        auto value = mime.imageData();
        job.image = qvariant_cast<QImage>(value);
        if (job.image.isNull() && value.canConvert<QPixmap>())
            job.image = qvariant_cast<QPixmap>(value).toImage();
    }
    nextEncode_ = std::move(job);
    encode();
}

void ClipboardImages::encode()
{
    if (encoding_ || !nextEncode_)
        return;
    auto job = std::move(*nextEncode_);
    nextEncode_.reset();
    encoding_ = true;
    auto *watch = new QFutureWatcher<QByteArray>(this);
    connect(watch, &QFutureWatcher<QByteArray>::finished, this, [this, watch, rev = job.revision] {
        encoding_ = false;
        data_.encodedImage(rev, watch->result());
        watch->deleteLater();
        encode();
    });
    watch->setFuture(QtConcurrent::run([job = std::move(job)]() mutable -> QByteArray {
        if (!job.png.isEmpty()) {
            bool ok = false;
            readPng(job.png, true, ok);
            return ok ? std::move(job.png) : QByteArray{};
        }
        if (!allowed(job.image.size()))
            return {};
        QBuffer buffer(&job.png);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "png");
        writer.setCompression(20); // Qt's PNG plugin maps 0..100 to zlib compression levels.
        if (!writer.write(job.image) || job.png.size() > SessionData::maxImageBytes)
            return {};
        return std::move(job.png);
    }));
}

void ClipboardImages::decode()
{
    if (decoding_ || !nextDecode_)
        return;
    auto job = std::move(*nextDecode_);
    nextDecode_.reset();
    decoding_ = true;
    auto *watch = new QFutureWatcher<QImage>(this);
    connect(watch, &QFutureWatcher<QImage>::finished, this, [this, watch, rev = job.revision] {
        decoding_ = false;
        if (data_.expectsImage(rev)) {
            auto image = watch->result();
            const bool installed = !image.isNull() && install_(rev, image);
            data_.finishImage(rev, installed);
        }
        watch->deleteLater();
        decode();
    });
    watch->setFuture(QtConcurrent::run([png = std::move(job.png)]() mutable {
        bool ok = false;
        return readPng(png, false, ok);
    }));
}
} // namespace wwmm
