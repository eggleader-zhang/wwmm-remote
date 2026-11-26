#include "desktop_viewer.h"
#include "input_window.h"
#include "transfer_bar.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWindow>

namespace wwmm {
DesktopViewer::DesktopViewer(QWidget *parent, bool interactive, bool rich)
    : QDialog(parent, Qt::Window), surface_(new InputWindow(interactive))
{
    column_ = new QVBoxLayout(this);
    // The native renderer owns the video surface. Only the toolbar needs Qt's raster backing store.
    video_ = QWidget::createWindowContainer(surface_, this);
    video_->setFocusPolicy(interactive ? Qt::StrongFocus : Qt::NoFocus);
    video_->setMinimumSize(320, 180);
    pages_ = new QStackedWidget(this);
    pages_->addWidget(video_);
    auto *offline = new QWidget(pages_);
    auto *actions = new QVBoxLayout(offline);
    interrupted_ = new QLabel(offline);
    interrupted_->setTextFormat(Qt::PlainText);
    interrupted_->setWordWrap(true);
    retry_ = new QPushButton(tr("重新连接"), offline);
    retry_->setAutoDefault(false);
    actions->addStretch();
    actions->addWidget(interrupted_, 0, Qt::AlignCenter);
    actions->addWidget(retry_, 0, Qt::AlignCenter);
    actions->addStretch();
    pages_->addWidget(offline);
    column_->addWidget(pages_, 1);
    connect(retry_, &QPushButton::clicked, this, &DesktopViewer::reconnectRequested);
    auto *row = new QHBoxLayout;
    status_ = new QLabel(tr("等待第一帧…"), this);
    row->addWidget(status_, 1);
    displays_ = new QComboBox(this);
    displays_->setAccessibleName(tr("远端显示器"));
    displays_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    displays_->setMinimumContentsLength(12);
    row->addWidget(displays_);
    displays_->hide();
    connect(displays_, &QComboBox::activated, this, [this](int index) {
        emit displayRequested(displays_->itemData(index).toULongLong());
        focusVideo();
    });
    {
        quality_ = new QComboBox(this);
        quality_->setEnabled(false);
        quality_->setAccessibleName(tr("最高画质"));
        quality_->setToolTip(tr("最高传输分辨率，保留屏幕宽高比"));
        quality_->addItems({tr("1080p · 办公"), tr("1440p"), tr("4K")});
        row->addWidget(quality_);
        connect(quality_, &QComboBox::activated, this, [this](int index) {
            emit qualityRequested(VideoQuality(index));
            focusVideo();
        });
        sound_ = new QCheckBox(tr("远端声音"), this);
        sound_->setToolTip(tr("共享端同意后，播放对方电脑的系统声音"));
        row->addWidget(sound_);
        connect(sound_, &QCheckBox::toggled, this, [this](bool enabled) {
            emit soundRequested(enabled);
            focusVideo();
        });
    }
    connect(surface_, &InputWindow::fileDropped, this, &DesktopViewer::fileDropped);
    if (interactive) {
        auto *preedit = new QLabel(this);
        preedit->setTextFormat(Qt::PlainText);
        preedit->hide();
        row->addWidget(preedit);
        connect(surface_, &InputWindow::preeditChanged, preedit, [preedit](const QString &value) {
            preedit->setText(preedit->fontMetrics().elidedText(value, Qt::ElideRight, 220));
            preedit->setVisible(!value.isEmpty());
        });
        auto *switchApp = switchApp_ = new QPushButton(tr("切换远端应用"), this);
        switchApp->setEnabled(false);
        connect(surface_, &InputWindow::firstPictureShown, switchApp,
                [switchApp] { switchApp->setEnabled(true); });
        switchApp->setAutoDefault(false);
        row->addWidget(switchApp);
        connect(switchApp, &QPushButton::clicked, this, [this] {
            emit switchRequested();
            focusVideo();
        });
    }
    auto *end = new QPushButton(interactive ? tr("结束控制") : tr("结束查看"), this);
    end->setAutoDefault(false);
    row->addWidget(end);
    column_->addLayout(row);
    connect(end, &QPushButton::clicked, this, &QDialog::accept);
    showDesktop(rich);
    resize(960, 600);
}

void DesktopViewer::setDisplays(const QList<DesktopDisplay> &list, quint64 selected)
{
    QSignalBlocker block(displays_);
    displays_->clear();
    for (const auto &d : list) {
        const auto title = tr("%1 · %2 · %3 × %4%5")
                               .arg(displays_->count() + 1)
                               .arg(d.name)
                               .arg(d.pixels.width())
                               .arg(d.pixels.height())
                               .arg(d.primary ? tr(" · 主屏幕") : QString{});
        displays_->addItem(title, QVariant::fromValue(d.id));
    }
    displays_->setCurrentIndex(displays_->findData(QVariant::fromValue(selected)));
    displays_->setVisible(list.size() > 1);
    if (quality_)
        quality_->setEnabled(selected != 0);
}

void DesktopViewer::showQuality(VideoQuality limit, VideoQuality active)
{
    const QStringList titles{tr("1080p · 办公"), tr("1440p"), tr("4K")};
    const QStringList rates{tr("720p"), tr("1080p"), tr("1440p"), tr("4K")};
    for (int i = 0; i < titles.size(); ++i)
        quality_->setItemText(i, i == int(limit) && active != limit
                                    ? tr("%1（当前 %2）").arg(titles[i], rates[int(active) + 1])
                                    : titles[i]);
    quality_->setCurrentIndex(int(limit));
}

void DesktopViewer::showInterrupted(const QString &message, bool canRetry)
{
    pages_->setCurrentIndex(1);
    interrupted_->setText(message);
    retry_->setEnabled(canRetry);
    displays_->hide();
    quality_->hide();
    sound_->hide();
    if (switchApp_)
        switchApp_->setEnabled(false);
}

void DesktopViewer::showDesktop(bool rich)
{
    pages_->setCurrentIndex(0);
    quality_->setVisible(rich);
    sound_->setVisible(rich);
    showQuality(VideoQuality::FullHD, VideoQuality::FullHD);
    setDisplays({}, 0);
    showSound(false, false);
    setStatus(tr("等待第一帧…"));
}

void DesktopViewer::showSound(bool local, bool peer)
{
    if (!sound_)
        return;
    QSignalBlocker guard(sound_);
    sound_->setChecked(local);
    sound_->setText(local && !peer ? tr("等待共享端开启声音") : tr("远端声音"));
}

void DesktopViewer::addTools(QWidget *tools)
{
    column_->addWidget(tools);
}

void DesktopViewer::allowFileDrops(bool allow)
{
    surface_->allowFileDrops(allow);
    setAcceptDrops(allow);
}

void DesktopViewer::dragEnterEvent(QDragEnterEvent *event)
{
    if (!droppedFile(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void DesktopViewer::dropEvent(QDropEvent *event)
{
    const auto path = droppedFile(event->mimeData());
    if (!path.isEmpty()) {
        event->acceptProposedAction();
        emit fileDropped(path);
    }
}

quintptr DesktopViewer::videoView() const
{
    return quintptr(surface_->winId());
}

void DesktopViewer::focusVideo()
{
    video_->setFocus(Qt::OtherFocusReason);
    surface_->requestActivate();
}

void DesktopViewer::setStatus(const QString &text)
{
    status_->setText(text);
}
} // namespace wwmm
