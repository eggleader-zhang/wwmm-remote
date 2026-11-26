#include "desktop_viewer.h"
#include "input_window.h"
#include "transfer_bar.h"
#include <QComboBox>
#include <QCheckBox>
#include <QActionGroup>
#include <QDragEnterEvent>
#include <QGridLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

namespace wwmm {
DesktopViewer::DesktopViewer(QWidget *parent, bool interactive, bool rich)
    : QDialog(parent, Qt::Window), surface_(new InputWindow(interactive))
{
    column_ = new QVBoxLayout(this);
    // The native renderer owns the video surface. Only the toolbar needs Qt's raster backing store.
    pages_ = new QStackedWidget(this);
    picturePage_ = new QWidget(pages_);
    auto *grid = new QGridLayout(picturePage_);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    grid->setColumnStretch(0, 1);
    grid->setRowStretch(0, 1);
    video_ = QWidget::createWindowContainer(surface_, picturePage_);
    video_->setFocusPolicy(interactive ? Qt::StrongFocus : Qt::NoFocus);
    video_->setMinimumSize(320, 180);
    grid->addWidget(video_, 0, 0);
    horizontal_ = new QScrollBar(Qt::Horizontal, picturePage_);
    horizontal_->setObjectName("horizontalPan");
    vertical_ = new QScrollBar(Qt::Vertical, picturePage_);
    horizontal_->setAccessibleName(tr("水平查看画面"));
    vertical_->setAccessibleName(tr("垂直查看画面"));
    grid->addWidget(horizontal_, 1, 0);
    grid->addWidget(vertical_, 0, 1);
    for (auto *bar : {horizontal_, vertical_}) {
        bar->hide();
        bar->setFocusPolicy(Qt::NoFocus);
        connect(bar, &QScrollBar::valueChanged, this, &DesktopViewer::applyViewport);
    }
    pages_->addWidget(picturePage_);
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
    status_->setWordWrap(true);
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
    auto *displayOptions = new QToolButton(this);
    displayOptions->setText(tr("显示"));
    displayOptions->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(displayOptions);
    displayOptions->setMenu(menu);
    auto *scale = new QActionGroup(menu);
    auto *fit = menu->addAction(tr("适应窗口"));
    original_ = menu->addAction(tr("原始像素（1:1）"));
    original_->setObjectName("originalPixels");
    for (auto *action : {fit, original_}) {
        action->setCheckable(true);
        scale->addAction(action);
    }
    fit->setChecked(true);
    connect(scale, &QActionGroup::triggered, this, [this] {
        updateViewport();
        focusVideo();
    });
    menu->addSeparator();
    auto *full = menu->addAction(tr("全屏"));
    full->setObjectName("toggleFullScreen");
    connect(menu, &QMenu::aboutToShow, this, [this, full] {
        full->setText(isFullScreen() ? tr("退出全屏") : tr("全屏"));
    });
    connect(full, &QAction::triggered, this, &DesktopViewer::toggleFullScreen);
    row->addWidget(displayOptions);

    auto *layoutTimer = new QTimer(this);
    layoutTimer->setSingleShot(true);
    auto resized = [layoutTimer] { layoutTimer->start(0); };
    connect(surface_, &QWindow::widthChanged, this, resized);
    connect(surface_, &QWindow::heightChanged, this, resized);
    connect(surface_, &InputWindow::pixelRatioChanged, this, resized);
    connect(surface_, &InputWindow::pictureSizeChanged, this, &DesktopViewer::updateViewport);
    connect(layoutTimer, &QTimer::timeout, this, &DesktopViewer::updateViewport);
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

void DesktopViewer::updateViewport()
{
    const auto picture = surface_->pictureSize();
    const auto page = picturePage_->size();
    const auto scale = surface_->devicePixelRatio();
    const int barWidth = vertical_->sizeHint().width(), barHeight = horizontal_->sizeHint().height();
    const bool original = original_->isChecked() && !picture.isEmpty();
    bool horizontal = original && picture.width() > qRound(page.width() * scale);
    const bool vertical = original && picture.height() > qRound((page.height() - (horizontal ? barHeight : 0)) * scale);
    horizontal = horizontal || (vertical && picture.width() > qRound((page.width() - barWidth) * scale));
    auto setBar = [](QScrollBar *bar, bool show, int span, int pixels) {
        QSignalBlocker block(bar);
        bar->setVisible(show);
        bar->setPageStep(span);
        bar->setRange(0, show ? qMax(0, pixels - span) : 0);
    };
    setBar(horizontal_, horizontal, qRound((page.width() - (vertical ? barWidth : 0)) * scale), picture.width());
    setBar(vertical_, vertical, qRound((page.height() - (horizontal ? barHeight : 0)) * scale), picture.height());
    applyViewport();
}

void DesktopViewer::applyViewport()
{
    surface_->setViewport({original_->isChecked(), {horizontal_->value(), vertical_->value()}});
    emit viewportChanged();
}

void DesktopViewer::toggleFullScreen()
{
    setWindowState(windowState().setFlag(Qt::WindowFullScreen, !isFullScreen()));
    if (pages_->currentWidget() == picturePage_)
        focusVideo();
}

void DesktopViewer::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && isFullScreen()) {
        toggleFullScreen();
        event->accept();
    } else
        QDialog::keyPressEvent(event);
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
