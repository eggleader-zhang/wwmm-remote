#include "transfer_bar.h"
#include "clipboard_images.h"
#include "desktop_viewer.h"
#include "native_input.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <utility>

namespace wwmm {
namespace {
constexpr auto autoSyncSetting = "clipboard/autoSync";

QString downloadDirectory()
{
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return dir.isEmpty() ? QDir::homePath() : dir;
}
} // 匿名命名空间

QStringList droppedFiles(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls())
        return {};
    QStringList paths;
    for (const auto &url : mime->urls()) {
        if (!url.isLocalFile())
            continue;
        const auto path = url.toLocalFile();
        if (QFileInfo(path).isFile())
            paths.append(path);
    }
    return paths;
}

QString droppedFile(const QMimeData *mime)
{
    return droppedFiles(mime).value(0);
}

QString uniqueDownloadPath(const QString &dir, const QString &fileName)
{
    const QDir folder(dir);
    const auto name = QFileInfo(fileName).fileName();
    const auto dot = name.lastIndexOf(u'.');
    const auto base = dot > 0 ? name.left(dot) : name;
    const auto suffix = dot > 0 ? name.mid(dot) : QString{};
    auto path = folder.filePath(name);
    for (quint64 number = 1; QFileInfo::exists(path) || QFileInfo(path).isSymLink(); ++number)
        path = folder.filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(number).arg(suffix));
    return path;
}

bool TransferBar::autoClipboardSync()
{
    return QSettings().value(autoSyncSetting, false).toBool();
}

void TransferBar::setAutoClipboardSync(bool enabled)
{
    QSettings().setValue(autoSyncSetting, enabled);
}

TransferBar::TransferBar(SessionData &data, QWidget *parent, DesktopViewer *viewer) : QWidget(parent), data_(data)
{
    setAcceptDrops(true);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    clipboard_ = new QAction(this);
    clipboard_->setObjectName("sessionClipboardSync");
    clipboard_->setCheckable(true);
    if (viewer) {
        // 动作由传输栏持有，断线时随传输栏移除。
        send_ = new QAction(tr("发送文件…"), this);
        send_->setObjectName("sessionSendFile");
        viewer->insertSessionAction(clipboard_);
        viewer->insertSessionAction(send_);
        auto *autoSync = new QAction(tr("连接时自动开启剪贴板同步"), this);
        autoSync->setObjectName("autoClipboardSync");
        autoSync->setCheckable(true);
        autoSync->setChecked(autoClipboardSync());
        viewer->insertSessionAction(autoSync);
        connect(autoSync, &QAction::toggled, this, &TransferBar::setAutoClipboardSync);
        connect(send_, &QAction::triggered, this, [this, viewer] {
            if (dialog_)
                return;
            chooseFile(false);
            if (dialog_)
                connect(dialog_, &QFileDialog::finished, viewer, &DesktopViewer::focusVideo);
        });
        row->setContentsMargins(10, 0, 10, 4);
    } else {
        auto *check = new QCheckBox(this);
        row->addWidget(check);
        connect(check, &QCheckBox::clicked, &data, &SessionData::enableClipboard);
        connect(clipboard_, &QAction::changed, check, [this, check] {
            check->setText(clipboard_->text());
            check->setToolTip(clipboard_->toolTip());
            check->setChecked(clipboard_->isChecked());
            check->setEnabled(clipboard_->isEnabled());
        });
    }
    status_ = new QLabel(this);
    status_->setObjectName("transferStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    row->addWidget(status_, 1);
    file_ = new QPushButton(tr("发送文件"), this);
    file_->setObjectName("chooseTransferFile");
    download_ = new QPushButton(tr("保存到下载"), this);
    download_->setObjectName("saveToDownloads");
    cancel_ = new QPushButton(tr("取消传输"), this);
    reveal_ = new QPushButton(tr("打开所在位置"), this);
    reveal_->setObjectName("revealSavedFile");
    close_ = new QPushButton(tr("关闭"), this);
    close_->setObjectName("closeSavedFile");
    for (auto *button : {file_, download_, cancel_, reveal_, close_}) {
        button->setAutoDefault(false);
        row->addWidget(button);
    }
    connect(clipboard_, &QAction::triggered, &data, &SessionData::enableClipboard);
    connect(file_, &QPushButton::clicked, this,
            [this] { chooseFile(data_.fileState() == SessionData::Offered); });
    connect(download_, &QPushButton::clicked, this, &TransferBar::saveToDownloads);
    connect(cancel_, &QPushButton::clicked, &data, &SessionData::cancelFile);
    connect(reveal_, &QPushButton::clicked, this, [this] {
        if (completedPath_.isEmpty())
            return;
#ifdef Q_OS_MACOS
        QProcess::startDetached("open", {"-R", completedPath_});
#elif defined(Q_OS_WIN)
        QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(completedPath_)});
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(completedPath_).absolutePath()));
#endif
    });
    auto closeCompletion = [this] {
        completionTimer_.stop();
        completedPath_.clear();
        update();
    };
    completionTimer_.setSingleShot(true);
    completionTimer_.setInterval(10000);
    connect(&completionTimer_, &QTimer::timeout, this, closeCompletion);
    connect(close_, &QPushButton::clicked, this, closeCompletion);
    queueTimer_.setSingleShot(true);
    connect(&queueTimer_, &QTimer::timeout, this, &TransferBar::sendNextFile);
    connect(&data, &SessionData::changed, this, &TransferBar::update);
    connect(&data, &SessionData::clipboardSettled, this, &TransferBar::update);
    progressTimer_.setInterval(120);
    connect(&progressTimer_, &QTimer::timeout, this, &TransferBar::updateTransferStatus);
    connect(&data, &SessionData::notice, this, &TransferBar::showNotice);
    poll_.setInterval(180);
    connect(&poll_, &QTimer::timeout, this, &TransferBar::pollClipboard);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &TransferBar::pollClipboard);
    images_ = new ClipboardImages(
        data,
        [this](quint64 rev, const QImage &image) {
            pollClipboard();
            if (!data_.expectsImage(rev))
                return false;
            applying_ = true;
            previous_.reset();
            QApplication::clipboard()->setImage(image);
            revision_ = clipboardRevision();
            applying_ = false;
            emit clipboardSynced(tr("已同步远端截图"));
            return true;
        },
        this);
    connect(&data, &SessionData::clipboardText, this, [this](const QString &value) {
        applying_ = true;
        previous_ = value;
        QApplication::clipboard()->setText(value);
        revision_ = clipboardRevision();
        applying_ = false;
        emit clipboardSynced(tr("已同步远端文字"));
    });
    update();
}

void TransferBar::pollClipboard()
{
    if (!watching_ || applying_ || !data_.clipboardEnabled())
        return;
    const auto revision = clipboardRevision();
    if (revision == revision_)
        return;
    revision_ = revision;
    auto *mime = QApplication::clipboard()->mimeData();
    if (!mime)
        return;
    const auto urls = mime->urls();
    if (std::any_of(urls.cbegin(), urls.cend(), [](const QUrl &url) { return url.isLocalFile(); })) {
        previous_.reset();
        if (data_.supportsImages())
            data_.discardLocalCopy();
        return;
    }
    const bool plain = mime->hasText() && (!mime->hasUrls() || mime->hasFormat(QStringLiteral("text/plain")));
    const auto text = plain ? mime->text() : QString{};
    if (data_.supportsImages() && (mime->hasImage() || mime->hasFormat(QStringLiteral("image/png"))) &&
        text.isEmpty()) {
        previous_.reset();
        images_->copy(*mime);
        return;
    }
    if (!plain) {
        previous_.reset();
        if (data_.supportsImages())
            data_.discardLocalCopy();
        return;
    }
    if (previous_ && text == *previous_)
        return;
    if (text.size() <= SessionData::maxTextBytes)
        previous_ = text;
    data_.copyText(text);
}

void TransferBar::update()
{
    const bool ready = data_.ready();
    const bool firstReady = ready && !clipboardReady_;
    clipboardReady_ = ready;
    if (firstReady && autoClipboardSync() && !data_.localClipboard()) {
        data_.enableClipboard(true);
        return;
    }
    clipboard_->setEnabled(ready);
    const auto name = data_.supportsImages() ? tr("同步文字和截图") : tr("同步文字剪贴板");
    const bool waiting = data_.localClipboard() && !data_.clipboardEnabled();
    clipboard_->setText(send_ && waiting ? tr("%1（等待对方开启）").arg(name) : name);
    clipboard_->setToolTip(data_.supportsImages() ? tr("双方开启后，同步本次会话中新复制的文字和图片。")
                                                  : tr("双方开启后，同步本次会话中新复制的文字。"));
    clipboard_->setChecked(data_.localClipboard());
    if (watching_ != data_.clipboardEnabled()) {
        watching_ = data_.clipboardEnabled();
        previous_.reset();
        if (watching_) {
            revision_ = clipboardRevision();
            poll_.start();
        } else
            poll_.stop();
    }
    const auto state = data_.fileState();
    if (!ready || (state == SessionData::Offered && lastFileState_ != state)) {
        receivePath_.clear();
        receiveNotice_.clear();
    }
    lastFileState_ = state;
    if (!ready) {
        queueTimer_.stop();
        sendQueue_.clear();
        batchTotal_ = batchIndex_ = 0;
    } else if (state == SessionData::NoFile) {
        if (sendQueue_.isEmpty())
            batchTotal_ = batchIndex_ = 0;
        if ((!sendQueue_.isEmpty() || !receivePath_.isEmpty()) && !queueTimer_.isActive())
            queueTimer_.start(0);
    }
    if (!ready || state != SessionData::NoFile) {
        completionTimer_.stop();
        completedPath_.clear();
    }
    const bool complete = !completedPath_.isEmpty();
    const bool offered = state == SessionData::Offered;
    const bool transferring = ready && (state == SessionData::Sending || state == SessionData::Receiving);
    if (transferring && !progressTimer_.isActive()) {
        transferClock_.start();
        progressTimer_.start();
    } else if (!transferring) {
        progressTimer_.stop();
        transferClock_.invalidate();
    }
    if (send_) {
        send_->setEnabled(ready && state == SessionData::NoFile);
        setVisible(state != SessionData::NoFile || complete || data_.imagePending());
    }
    if (dialog_ && (!ready || (saving_ && (!offered || !receivePath_.isEmpty()))))
        dialog_->reject();
    const bool canSave = ready && offered && receivePath_.isEmpty();
    file_->setVisible(offered || (!send_ && !complete));
    file_->setEnabled(canSave || (ready && state == SessionData::NoFile));
    file_->setText(offered ? tr("另存为…") : tr("发送文件"));
    download_->setVisible(offered);
    download_->setEnabled(canSave);
    cancel_->setVisible(state != SessionData::NoFile);
    reveal_->setVisible(complete);
    close_->setVisible(complete);
    updateTransferStatus();
}

void TransferBar::updateTransferStatus()
{
    const auto state = data_.fileState();
    if (state == SessionData::NoFile) {
        if (!completedPath_.isEmpty()) {
            status_->setText(tr("已保存 %1").arg(QFileInfo(completedPath_).fileName()));
            return;
        }
        if (data_.imagePending()) {
            status_->setText(tr("正在同步截图…"));
            return;
        }
        status_->setText(!data_.ready() ? tr("文件与剪贴板未连接")
                        : data_.localClipboard() && !watching_ ? tr("等待对方开启剪贴板") : tr("可拖入文件发送"));
        return;
    }
    const auto fileName = fontMetrics().elidedText(data_.fileName(), Qt::ElideMiddle, 175);
    const QLocale locale;
    const auto total = data_.fileSize();
    const auto size = locale.formattedDataSize(total);
    QString status;
    if (state == SessionData::Offered)
        status = tr("对方发来 %1（%2）").arg(fileName, size);
    else if (state == SessionData::Offering)
        status = tr("等待对方接收 %1").arg(fileName);
    else {
        const auto bytes = data_.progress();
        const auto progress = locale.formattedDataSize(bytes);
        const double rate = transferClock_.isValid()
                                ? double(bytes) * 1000.0 / double(qMax<qint64>(1, transferClock_.elapsed())) : 0.0;
        const auto speed = tr("%1/s").arg(locale.formattedDataSize(qRound64(rate), 1, QLocale::DataSizeSIFormat));
        auto remaining = tr("计算剩余时间…");
        if (rate > 0.0) {
            const auto seconds = std::ceil(double(total - bytes) / rate);
            remaining = tr("约 %1 秒").arg(locale.toString(seconds, 'f', 0));
        }
        status = tr("%1 %2 · %3 / %4 · %5 · %6")
                     .arg(state == SessionData::Sending ? tr("发送") : tr("接收"), fileName, progress, size, speed,
                          remaining);
    }
    if (batchIndex_ && (state == SessionData::Offering || state == SessionData::Sending))
        status = tr("(%1/%2) %3").arg(batchIndex_).arg(batchTotal_).arg(status);
    status_->setText(status);
}

void TransferBar::chooseFile(bool saving)
{
    if (dialog_ || !data_.ready() ||
        (saving && (data_.fileState() != SessionData::Offered || !receivePath_.isEmpty())))
        return;
    saving_ = saving;
    auto *dialog = new QFileDialog(this, saving ? tr("保存收到的文件") : tr("选择要发送的文件"));
    dialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(saving ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    dialog->setFileMode(saving ? QFileDialog::AnyFile : QFileDialog::ExistingFiles);
    dialog->setWindowModality(Qt::WindowModal);
    if (saving) {
        dialog->setDirectory(downloadDirectory());
        dialog->selectFile(data_.fileName());
    }
    connect(dialog, &QFileDialog::finished, this, [this, dialog, saving](int result) {
        dialog_ = nullptr;
        if (result != QDialog::Accepted || !data_.ready())
            return;
        auto paths = dialog->selectedFiles();
        if (paths.isEmpty())
            return;
        if (saving)
            acceptFile(paths.first());
        else
            sendFiles(paths);
    });
    dialog->open();
}

void TransferBar::sendFile(const QString &path)
{
    sendFiles({path});
}

void TransferBar::sendFiles(const QStringList &paths)
{
    if (!data_.ready())
        return;
    for (const auto &path : paths) {
        const QFileInfo file(path);
        if (file.isFile()) {
            sendQueue_.append(file.absoluteFilePath());
            ++batchTotal_;
        }
    }
    update();
}

void TransferBar::sendNextFile()
{
    if (!data_.ready() || data_.fileState() != SessionData::NoFile)
        return;
    receivePath_.clear();
    receiveNotice_.clear();
    if (!sendQueue_.isEmpty()) {
        ++batchIndex_;
        data_.offerFile(sendQueue_.takeFirst());
    }
}

void TransferBar::acceptFile(const QString &path)
{
    if (path.isEmpty() || data_.fileState() != SessionData::Offered || !receivePath_.isEmpty())
        return;
    receivePath_ = QFileInfo(path).absoluteFilePath();
    // 文件通道校验并提交后会发出这条提示，取消或写入失败时不显示保存结果。
    receiveNotice_ = SessionData::tr("已校验并保存 %1。").arg(data_.fileName());
    data_.acceptFile(receivePath_);
    update();
}

void TransferBar::saveToDownloads()
{
    if (data_.fileState() != SessionData::Offered || !receivePath_.isEmpty())
        return;
    const auto dir = downloadDirectory();
    if (!QDir().mkpath(dir)) {
        status_->setText(tr("无法创建下载目录。"));
        return;
    }
    acceptFile(uniqueDownloadPath(dir, data_.fileName()));
}

void TransferBar::showNotice(const QString &message)
{
    if (data_.fileState() == SessionData::NoFile && !receivePath_.isEmpty()) {
        auto path = std::exchange(receivePath_, {});
        if (message == std::exchange(receiveNotice_, {})) {
            completedPath_ = std::move(path);
            completionTimer_.start();
            update();
            return;
        }
    }
    if (completedPath_.isEmpty())
        status_->setText(message);
}

void TransferBar::dragEnterEvent(QDragEnterEvent *event)
{
    if (data_.ready() && data_.fileState() == SessionData::NoFile &&
        !droppedFiles(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void TransferBar::dropEvent(QDropEvent *event)
{
    const auto paths = droppedFiles(event->mimeData());
    if (!paths.isEmpty() && data_.ready() && data_.fileState() == SessionData::NoFile) {
        sendFiles(paths);
        event->acceptProposedAction();
    }
}
} // namespace wwmm
