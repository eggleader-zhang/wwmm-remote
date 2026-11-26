#include "desktop_viewer.h"
#include "input_window.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWindow>

namespace wwmm {
DesktopViewer::DesktopViewer(QWidget *parent, bool interactive)
    : QDialog(parent, Qt::Window), surface_(new InputWindow(interactive))
{
    auto *column = new QVBoxLayout(this);
    // The native renderer owns the video surface. Only the toolbar needs Qt's raster backing store.
    video_ = QWidget::createWindowContainer(surface_, this);
    video_->setFocusPolicy(interactive ? Qt::StrongFocus : Qt::NoFocus);
    video_->setMinimumSize(320, 180);
    column->addWidget(video_, 1);
    auto *row = new QHBoxLayout;
    status_ = new QLabel(tr("等待第一帧…"), this);
    row->addWidget(status_, 1);
    if (interactive) {
        auto *preedit = new QLabel(this);
        preedit->setTextFormat(Qt::PlainText);
        preedit->hide();
        row->addWidget(preedit);
        connect(surface_, &InputWindow::preeditChanged, preedit, [preedit](const QString &value) {
            preedit->setText(preedit->fontMetrics().elidedText(value, Qt::ElideRight, 220));
            preedit->setVisible(!value.isEmpty());
        });
        auto *switchApp = new QPushButton(tr("切换远端应用"), this);
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
    column->addLayout(row);
    connect(end, &QPushButton::clicked, this, &QDialog::accept);
    resize(960, 600);
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
