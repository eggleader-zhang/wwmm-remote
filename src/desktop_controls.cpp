#include "desktop_controls.h"
#include "desktop_viewer.h"
#include "input_window.h"
#include "video_packets.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMutex>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <atomic>

namespace wwmm {
struct DesktopControls::Callbacks {
    QMutex mutex;
    DesktopControls *target;
    std::atomic_int queued{0}, presented{0};
    std::atomic<quint32> displayedStamp{0};
    std::optional<quint32> receivedStamp;
    std::atomic_bool needsIdr{false}, firstPicture{true};

    explicit Callbacks(DesktopControls *target) : target(target) {}

    static void post(std::shared_ptr<Callbacks> gate, std::function<void(DesktopControls *)> action)
    {
        QMutexLocker lock(&gate->mutex);
        if (!gate->target)
            return;
        QMetaObject::invokeMethod(
            gate->target,
            [gate, action = std::move(action)] {
                // The target is cleared on the GUI thread before either native backend stops.
                if (gate->target)
                    action(gate->target);
            },
            Qt::QueuedConnection);
    }

    static MediaError error(std::shared_ptr<Callbacks> gate)
    {
        return
            [gate](QString error) { post(gate, [error](DesktopControls *ui) { ui->session_.stop(error); }); };
    }
};

DesktopControls::DesktopControls(DesktopSession &session, QWidget *parent)
    : QWidget(parent), session_(session)
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    label_ = new QLabel(this);
    label_->setTextFormat(Qt::PlainText);
    label_->setWordWrap(true);
    yes_ = new QPushButton(tr("允许本次查看"), this);
    yes_->setObjectName("allowDesktop");
    stop_ = new QPushButton(this);
    stop_->setObjectName("endDesktop");
    row->addWidget(label_, 1);
    row->addWidget(yes_);
    row->addWidget(stop_);
    focusTimer_ = new QTimer(this);
    focusTimer_->setInterval(200);
    connect(focusTimer_, &QTimer::timeout, this, &DesktopControls::refreshInputFocus);
    connect(yes_, &QPushButton::clicked, this, [this] {
        if (session_.state() != DesktopSession::Requested)
            return;
        QString reason;
        if (session_.interactive() && !allowRemoteInput(reason))
            session_.stop(reason);
        else
            session_.accept();
    });
    connect(stop_, &QPushButton::clicked, this, [this] { session_.stop(); });
    connect(&session, &DesktopSession::changed, this, &DesktopControls::updateState);
    connect(&session, &DesktopSession::frame, this, [this](const QByteArray &frame, quint32 time) {
        if (player_) {
            if (session_.interactive() && !viewer_->inputSurface()->hasPictureSize())
                viewer_->inputSurface()->setPictureSize(h264PictureSize(frame));
            callbacks_->receivedStamp = time;
            player_->push(frame, time);
        }
    });
    connect(&session, &DesktopSession::inputReceived, this, [this](const InputEvent &event) {
        if (!input_)
            return;
        if (!input_->apply(event)) {
            session_.stop(tr("系统未能执行远程输入。请确认输入权限并返回普通桌面后重试。"));
            return;
        }
        if (event.kind == InputKind::ButtonUp || event.kind == InputKind::SwitchApp)
            QTimer::singleShot(15, this, &DesktopControls::refreshInputFocus);
    });
    connect(&session, &DesktopSession::focusReceived, this, [this](InputFocus focus) {
        if (viewer_)
            viewer_->inputSurface()->setInputFocus(focus);
    });
    connect(&session, &DesktopSession::bitrateChanged, this, [this](int rate) {
        if (capture_)
            capture_->setBitrate(rate);
    });
    connect(&session, &DesktopSession::keyframeNeeded, this, [this] {
        if (capture_)
            capture_->refresh();
    });
    hide();
}

DesktopControls::~DesktopControls()
{
    endMedia();
}

void DesktopControls::endMedia()
{
    focusTimer_->stop();
    input_.reset();
    inputSink_.reset();
    focusPending_ = false;
    lastFocus_.reset();
    if (callbacks_) {
        QMutexLocker lock(&callbacks_->mutex);
        callbacks_->target = nullptr;
    }
    capture_.reset();
    player_.reset();
    callbacks_.reset();
    if (viewer_) {
        viewer_->disconnect(this);
        viewer_->inputSurface()->disconnect(this);
        viewer_->hide();
        viewer_->deleteLater();
        viewer_ = nullptr;
    }
}

void DesktopControls::updateState()
{
    const auto s = session_.state();
    const bool control = session_.interactive();
    setVisible(s != DesktopSession::Idle);
    yes_->setVisible(s == DesktopSession::Requested);
    yes_->setText(control ? tr("允许本次控制") : tr("允许本次查看"));
    stop_->setText(s == DesktopSession::Requested ? tr("拒绝")
                   : s == DesktopSession::Sharing ? (control ? tr("停止控制") : tr("停止共享"))
                   : s == DesktopSession::Viewing ? (control ? tr("结束控制") : tr("结束查看"))
                                                  : tr("取消"));
    const auto name = session_.peerName();
    switch (s) {
    case DesktopSession::Idle:
        endMedia();
        break;
    case DesktopSession::Connecting:
        label_->setText(tr("正在验证设备身份…"));
        break;
    case DesktopSession::Waiting:
        label_->setText((control ? tr("等待 %1 同意控制主屏幕…") : tr("等待 %1 同意查看主屏幕…")).arg(name));
        break;
    case DesktopSession::Requested: {
        label_->setText(
            (control ? tr("%1 请求查看你的主屏幕，并使用键盘和鼠标。") : tr("%1 请求查看你的主屏幕。"))
                .arg(name));
        for (QWidget *parent = parentWidget(); parent; parent = parent->parentWidget())
            if (auto *tabs = qobject_cast<QTabWidget *>(parent)) {
                tabs->setCurrentIndex(0);
                break;
            }
        QApplication::alert(window());
        break;
    }
    case DesktopSession::Sharing:
        label_->setText(
            (control ? tr("%1 正在控制你的主屏幕和键鼠。") : tr("正在向 %1 共享主屏幕（只读）。")).arg(name));
        startCapture();
        if (control) {
            inputSink_ = createInputSink();
            input_ = std::make_unique<HostInput>(session_.peerOS(), localOS(), *inputSink_);
            focusTimer_->start();
            refreshInputFocus();
        }
        break;
    case DesktopSession::Viewing:
        label_->setText((control ? tr("正在控制 %1 的主屏幕。") : tr("正在查看 %1 的主屏幕。")).arg(name));
        openViewer();
        break;
    }
}

void DesktopControls::refreshInputFocus()
{
    if (!input_ || focusPending_)
        return;
    focusPending_ = true;
    auto *watcher = new QFutureWatcher<InputFocus>(this);
    connect(watcher, &QFutureWatcher<InputFocus>::finished, this, [this, watcher, gate = callbacks_] {
        const auto kind = watcher->result();
        watcher->deleteLater();
        if (!input_ || gate != callbacks_)
            return;
        focusPending_ = false;
        if (lastFocus_ != kind) {
            input_->setFocus(kind);
            lastFocus_ = kind;
            session_.reportFocus(kind);
        }
    });
    watcher->setFuture(QtConcurrent::run(&focusedInputKind));
}

void DesktopControls::startCapture()
{
    auto gate = callbacks_ = std::make_shared<Callbacks>(this);
    capture_ = captureDesktop(
        [gate](QByteArray data, quint32 stamp, bool key) {
            if (gate->queued.fetch_add(1) >= 3) {
                --gate->queued;
                if (!gate->needsIdr.exchange(true))
                    Callbacks::post(gate, [](DesktopControls *ui) {
                        if (ui->capture_)
                            ui->capture_->refresh();
                    });
                return;
            }
            if (gate->needsIdr && !key) {
                --gate->queued;
                return;
            }
            if (key)
                gate->needsIdr = false;
            Callbacks::post(gate, [gate, data = std::move(data), stamp, key](DesktopControls *ui) mutable {
                --gate->queued;
                ui->session_.sendFrame(std::move(data), stamp, key);
            });
        },
        Callbacks::error(gate));
}

void DesktopControls::openViewer()
{
    auto gate = callbacks_ = std::make_shared<Callbacks>(this);
    const bool control = session_.interactive();
    viewer_ = new DesktopViewer(this, control);
    viewer_->setWindowTitle((control ? tr("%1 · 控制桌面") : tr("%1 · 只读桌面")).arg(session_.peerName()));
    if (control) {
        auto *surface = viewer_->inputSurface();
        auto *resized = new QTimer(viewer_);
        resized->setSingleShot(true);
        auto resizeFinished = [resized] { resized->start(60); };
        connect(surface, &QWindow::widthChanged, resized, resizeFinished);
        connect(surface, &QWindow::heightChanged, resized, resizeFinished);
        connect(resized, &QTimer::timeout, this, [this, gate] {
            if (callbacks_ == gate && !gate->firstPicture)
                session_.refreshVideo();
        });
        connect(surface, &InputWindow::input, this, [this](const InputEvent &e) { session_.sendInput(e); });
        connect(viewer_, &DesktopViewer::switchRequested, this, [this] {
            InputEvent event;
            event.kind = InputKind::SwitchApp;
            session_.sendInput(event);
        });
    }
    connect(viewer_, &QDialog::finished, this, [this] { session_.stop(); });
    auto *timer = new QTimer(viewer_);
    timer->setInterval(1000);
    const auto mode = control ? tr("控制") : tr("只读");
    connect(timer, &QTimer::timeout, viewer_, [this, gate, mode, shown = false]() mutable {
        if (!viewer_ || callbacks_ != gate)
            return;
        if (!viewer_->isMinimized() && gate->receivedStamp &&
            qint32(*gate->receivedStamp - gate->displayedStamp.load()) != 0 && gate->presented.load() == 0)
            session_.refreshVideo();
        int count = gate->presented.exchange(0);
        if (count) {
            shown = true;
            viewer_->setStatus(tr("%1 · H.264 · 最近一秒显示 %2 帧").arg(mode).arg(count));
        } else if (shown)
            viewer_->setStatus(tr("%1 · H.264 · 保持当前画面").arg(mode));
    });
    timer->start();
    viewer_->show();
    if (control)
        viewer_->focusVideo();
    player_ = playDesktop(
        viewer_->videoView(),
        [gate, control](quint32 stamp) {
            gate->displayedStamp = stamp;
            ++gate->presented;
            if (control && gate->firstPicture.exchange(false))
                Callbacks::post(gate, [](DesktopControls *ui) { ui->viewer_->inputSurface()->presented(); });
        },
        Callbacks::error(gate));
}
} // namespace wwmm
