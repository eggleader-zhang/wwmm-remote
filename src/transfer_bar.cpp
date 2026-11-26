#include "transfer_bar.h"
#include "clipboard_images.h"
#include "native_input.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <algorithm>

namespace wwmm {
QString droppedFile(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls())
        return {};
    auto urls = mime->urls();
    if (urls.size() != 1 || !urls.first().isLocalFile())
        return {};
    return urls.first().toLocalFile();
}

TransferBar::TransferBar(SessionData &data, QWidget *parent) : QWidget(parent), data_(data)
{
    setAcceptDrops(true);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    clipboard_ = new QCheckBox(tr("同步文字剪贴板"), this);
    clipboard_->setToolTip(tr("双方勾选后，仅同步本次会话中新复制的文字。"));
    row->addWidget(clipboard_);
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    row->addWidget(status_, 1);
    file_ = new QPushButton(tr("发送文件"), this);
    cancel_ = new QPushButton(tr("取消传输"), this);
    for (auto *button : {file_, cancel_}) {
        button->setAutoDefault(false);
        row->addWidget(button);
    }
    connect(clipboard_, &QCheckBox::toggled, &data, &SessionData::enableClipboard);
    connect(file_, &QPushButton::clicked, this,
            [this] { chooseFile(data_.fileState() == SessionData::Offered); });
    connect(cancel_, &QPushButton::clicked, &data, &SessionData::cancelFile);
    connect(&data, &SessionData::changed, this, &TransferBar::update);
    connect(&data, &SessionData::clipboardSettled, this, &TransferBar::update);
    auto *progress = new QTimer(this);
    progress->setSingleShot(true);
    progress->setInterval(120);
    connect(&data, &SessionData::progressChanged, progress, [progress] {
        if (!progress->isActive())
            progress->start();
    });
    connect(progress, &QTimer::timeout, this, [this] {
        if (data_.fileState() == SessionData::Sending || data_.fileState() == SessionData::Receiving)
            update();
    });
    connect(&data, &SessionData::notice, status_, &QLabel::setText);
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
            return true;
        },
        this);
    connect(&data, &SessionData::clipboardText, this, [this](const QString &value) {
        applying_ = true;
        previous_ = value;
        QApplication::clipboard()->setText(value);
        revision_ = clipboardRevision();
        applying_ = false;
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
    clipboard_->setEnabled(ready);
    clipboard_->setText(data_.supportsImages() ? tr("同步文字和截图") : tr("同步文字剪贴板"));
    clipboard_->setToolTip(data_.supportsImages() ? tr("双方开启后，同步本次会话中新复制的文字和图片。")
                                                  : tr("双方开启后，同步本次会话中新复制的文字。"));
    {
        QSignalBlocker block(clipboard_);
        clipboard_->setChecked(data_.localClipboard());
    }
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
    if (dialog_ && (!ready || (saving_ && state != SessionData::Offered)))
        dialog_->reject();
    file_->setEnabled(ready && (state == SessionData::NoFile || state == SessionData::Offered));
    file_->setText(state == SessionData::Offered ? tr("选择保存位置") : tr("发送文件"));
    cancel_->setVisible(state != SessionData::NoFile);
    if (state == SessionData::NoFile) {
        if (data_.imagePending()) {
            status_->setText(tr("正在同步截图…"));
            return;
        }
        status_->setText(!ready ? tr("文件与剪贴板未连接")
                                : (data_.localClipboard() && !watching_ ? tr("等待对方开启剪贴板")
                                                                        : tr("可拖入一个文件发送")));
        return;
    }
    const auto name = fontMetrics().elidedText(data_.fileName(), Qt::ElideMiddle, 175);
    const auto size = QLocale().formattedDataSize(data_.fileSize());
    if (state == SessionData::Offered)
        status_->setText(tr("对方发来 %1（%2）").arg(name, size));
    else if (state == SessionData::Offering)
        status_->setText(tr("等待对方接收 %1").arg(name));
    else {
        const auto progress = QLocale().formattedDataSize(data_.progress());
        status_->setText(
            tr("%1 %2 · %3 / %4")
                .arg(state == SessionData::Sending ? tr("发送") : tr("接收"), name, progress, size));
    }
}

void TransferBar::chooseFile(bool saving)
{
    if (dialog_ || !data_.ready())
        return;
    saving_ = saving;
    auto *dialog = new QFileDialog(this, saving ? tr("保存收到的文件") : tr("选择要发送的文件"));
    dialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(saving ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    dialog->setFileMode(saving ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
    dialog->setWindowModality(Qt::WindowModal);
    if (saving) {
        dialog->setDirectory(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
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
            data_.acceptFile(paths.first());
        else
            data_.offerFile(paths.first());
    });
    dialog->open();
}

void TransferBar::dragEnterEvent(QDragEnterEvent *event)
{
    if (data_.ready() && data_.fileState() == SessionData::NoFile &&
        !droppedFile(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void TransferBar::dropEvent(QDropEvent *event)
{
    const auto path = droppedFile(event->mimeData());
    if (!path.isEmpty() && data_.ready() && data_.fileState() == SessionData::NoFile) {
        data_.offerFile(path);
        event->acceptProposedAction();
    }
}
} // namespace wwmm
