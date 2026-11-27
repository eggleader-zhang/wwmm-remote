#include "desktop_controls.h"
#include "desktop_viewer.h"
#include "desktop_tray.h"
#include "input_window.h"
#include "transfer_bar.h"
#include <QScreen>
#include <QVBoxLayout>
#include "video_packets.h"
#include <QApplication>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QHBoxLayout>
#include <QLabel>
#include <QMutex>
#include <QPushButton>
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
    std::optional<quint32> receivedStamp, capturedStamp;
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
        return [gate](QString error) {
            post(gate, [error](DesktopControls *ui) {
                if (!ui->session_.reduceQuality())
                    ui->session_.stop(error);
            });
        };
    }
};

DesktopControls::DesktopControls(DesktopSession &session, Discovery &discovery, QWidget *parent)
    : QWidget(parent), session_(session), discovery_(discovery)
{
    column_ = new QVBoxLayout(this);
    column_->setContentsMargins(0, 0, 0, 0);
    auto *row = new QHBoxLayout;
    column_->addLayout(row);
    label_ = new QLabel(this);
    label_->setTextFormat(Qt::PlainText);
    label_->setWordWrap(true);
    yes_ = new QPushButton(tr("允许本次查看"), this);
    yes_->setObjectName("allowDesktop");
    stop_ = new QPushButton(this);
    stop_->setObjectName("endDesktop");
    row->addWidget(label_, 1);
    soundCheck_ = new QCheckBox(tr("共享系统声音"), this);
    soundCheck_->hide();
    row->addWidget(soundCheck_);
    connect(soundCheck_, &QCheckBox::toggled, &session, &DesktopSession::enableSound);
    connect(&session, &DesktopSession::soundChanged, this, &DesktopControls::updateSound);
    connect(&session, &DesktopSession::qualityChanged, this, [this] {
        if (viewer_)
            viewer_->showQuality(session_.qualityLimit(), session_.quality());
    });
    connect(&session, &DesktopSession::notice, this, [this](const QString &message) {
        if (viewer_)
            viewer_->setStatus(message);
    });
    row->addWidget(yes_);
    row->addWidget(stop_);
    rememberAccess_ = new QCheckBox(tr("以后允许此设备免确认查看和控制本机"), this);
    rememberAccess_->setObjectName("rememberAccess");
    column_->addWidget(rememberAccess_);
    connect(rememberAccess_, &QCheckBox::toggled, this, [this](bool remember) {
        yes_->setText(remember ? tr("允许并记住") : tr("允许本次控制"));
    });
    focusTimer_ = new QTimer(this);
    focusTimer_->setInterval(200);
    connect(focusTimer_, &QTimer::timeout, this, &DesktopControls::refreshInputFocus);
    connect(yes_, &QPushButton::clicked, this, [this] {
        session_.accept(rememberAccess_->isChecked());
    });
    connect(stop_, &QPushButton::clicked, this, [this] { session_.stop(); });
    connect(&session, &DesktopSession::changed, this, &DesktopControls::updateState);
    connect(&discovery, &Discovery::changed, this, &DesktopControls::updateReconnect);
    connect(&session, &DesktopSession::displaySwitched, this, &DesktopControls::switchDisplay);
    connect(&session, &DesktopSession::displaysChanged, this, [this] {
        if (viewer_)
            viewer_->setDisplays(session_.displays(), session_.displayId());
    });
    connect(&session, &DesktopSession::beforePaste, this, [this] {
        if (transfer_)
            transfer_->pollClipboard();
    });
    topology_.setSingleShot(true);
    topology_.setInterval(120);
    auto topologyChanged = [this] {
        if (session_.state() == DesktopSession::Sharing)
            topology_.start();
    };
    auto watchScreen = [this, topologyChanged](QScreen *screen) {
        connect(screen, &QScreen::geometryChanged, this, topologyChanged);
        connect(screen, &QScreen::physicalDotsPerInchChanged, this, topologyChanged);
        connect(screen, &QScreen::orientationChanged, this, topologyChanged);
        topologyChanged();
    };
    for (auto *screen : QGuiApplication::screens())
        watchScreen(screen);
    connect(qApp, &QGuiApplication::screenAdded, this, watchScreen);
    connect(qApp, &QGuiApplication::screenRemoved, this, topologyChanged);
    connect(qApp, &QGuiApplication::primaryScreenChanged, this, topologyChanged);
    connect(&topology_, &QTimer::timeout, this, [this] { session_.updateHostDisplays(desktopDisplays()); });
    settleMotion_.setSingleShot(true);
    settleMotion_.setInterval(180);
    connect(&settleMotion_, &QTimer::timeout, this, [this] {
        if (capture_ && (!lastPolish_.isValid() || lastPolish_.elapsed() > 600)) {
            lastPolish_.start();
            capture_->refresh();
        }
    });
    frameTimer_.setInterval(1000);
    connect(&frameTimer_, &QTimer::timeout, this, [this] {
        const auto &gate = callbacks_;
        if (!gate || !viewer_ || !gate->receivedStamp) {
            frameTimer_.stop();
            return;
        }
        const int count = gate->presented.exchange(0);
        const bool outstanding = gate->firstPicture || *gate->receivedStamp != gate->displayedStamp.load();
        if (!count && outstanding && !viewer_->isMinimized())
            session_.refreshVideo();
        if (viewer_)
            viewer_->setFrameRate(count);
        if (!count && !outstanding)
            frameTimer_.stop();
    });
    connect(&session, &DesktopSession::frame, this, [this](const QByteArray &frame, quint32 time) {
        if (player_) {
            if (!callbacks_->receivedStamp || !viewer_->inputSurface()->hasPictureSize())
                viewer_->inputSurface()->setPictureSize(h264PictureSize(frame));
            callbacks_->receivedStamp = time;
            if (!frameTimer_.isActive())
                frameTimer_.start();
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
        if (capture_ && (event.kind == InputKind::KeyDown || event.kind == InputKind::Text ||
                         event.kind == InputKind::Wheel || event.kind == InputKind::ButtonDown))
            capture_->inputActivity();
        if (transfer_ && session_.dataLink().clipboardEnabled() && event.kind == InputKind::KeyDown &&
            (event.letter == 'C' || event.letter == 'X') && (event.mods & 0x99))
            QTimer::singleShot(16, transfer_, &TransferBar::pollClipboard);
        if (event.kind == InputKind::ButtonDown)
            dragging_ = true;
        if (event.kind == InputKind::Wheel || (dragging_ && event.kind == InputKind::Move) ||
            event.kind == InputKind::ButtonUp)
            settleMotion_.start();
        if (event.kind == InputKind::ButtonUp || event.kind == InputKind::Reset)
            dragging_ = false;
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

void DesktopControls::retireMedia(bool keepInput)
{
    if (!keepInput) {
        focusTimer_->stop();
        dragging_ = false;
        input_.reset();
        inputSink_.reset();
        lastFocus_.reset();
    }
    settleMotion_.stop();
    frameTimer_.stop();
    focusPending_ = false;
    if (callbacks_) {
        QMutexLocker lock(&callbacks_->mutex);
        callbacks_->target = nullptr;
    }
    capture_.reset();
    player_.reset();
    callbacks_.reset();
}

void DesktopControls::closeSound()
{
    if (soundCallbacks_) {
        QMutexLocker guard(&soundCallbacks_->mutex);
        soundCallbacks_->target = nullptr;
    }
    audio_.reset();
    soundCallbacks_.reset();
    audioStream_.reset();
}

void DesktopControls::updateSound()
{
    const bool local = session_.localSound(), peer = session_.peerSound();
    if (viewer_)
        viewer_->showSound(local, peer);
    {
        QSignalBlocker block(soundCheck_);
        soundCheck_->setChecked(local);
        soundCheck_->setText(!local && peer ? tr("对方请求系统声音")
                                            : (local && !peer ? tr("等待对方收听") : tr("共享系统声音")));
    }
    auto stream = session_.sound();
    if (audioStream_ == stream)
        return;
    closeSound();
    if (!stream)
        return;
    audioStream_ = stream;
    auto gate = soundCallbacks_ = std::make_shared<Callbacks>(this);
    audio_ = openSystemAudio(session_.state() == DesktopSession::Sharing, std::move(stream),
                             [gate](QString reason) {
                                 Callbacks::post(gate, [reason](DesktopControls *ui) {
                                     ui->session_.enableSound(false);
                                     emit ui->session_.notice(reason);
                                 });
                             });
}

void DesktopControls::endMedia(bool keepViewer)
{
    closeSound();
    retireMedia();
    topology_.stop();
    delete transfer_;
    transfer_ = nullptr;
    if (viewer_) {
        viewer_->allowFileDrops(false);
        if (keepViewer) {
            viewer_->inputSurface()->changeDisplay();
            viewer_->inputSurface()->setInputFocus(InputFocus::Raw);
            return;
        }
        viewer_->disconnect(this);
        viewer_->inputSurface()->disconnect(this);
        viewer_->hide();
        viewer_->deleteLater();
        viewer_ = nullptr;
    }
}

std::optional<NearbyDevice> DesktopControls::reconnectPeer() const
{
    const auto &devices = discovery_.devicesByKey();
    const auto peer = devices.constFind(session_.peerKey());
    if (peer != devices.cend() && peer->canRequestDesktop(session_.interactive()))
        return *peer;
    return {};
}

void DesktopControls::updateReconnect()
{
    if (!viewer_ || session_.state() != DesktopSession::Interrupted)
        return;
    const bool online = reconnectPeer().has_value();
    const auto text = online ? tr("%1 已上线，可以重新连接。")
                             : tr("正在等待 %1 回到局域网…");
    viewer_->showInterrupted(text.arg(session_.peerName()), online);
}

void DesktopControls::updateState()
{
    const auto s = session_.state();
    const bool control = session_.interactive();
    setVisible(s != DesktopSession::Idle);
    yes_->setVisible(s == DesktopSession::Requested);
    rememberAccess_->setVisible(s == DesktopSession::Requested && control);
    rememberAccess_->setChecked(false);
    soundCheck_->setVisible(s == DesktopSession::Sharing && session_.richMedia());
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
    case DesktopSession::Interrupted:
        endMedia(true);
        label_->setText(tr("与 %1 的会话已中断。").arg(name));
        updateReconnect();
        break;
    case DesktopSession::Connecting:
        label_->setText(tr("正在验证设备身份…"));
        if (viewer_)
            viewer_->showInterrupted(label_->text(), false);
        break;
    case DesktopSession::Waiting:
        label_->setText(tr("等待 %1 同意桌面请求…").arg(name));
        if (viewer_)
            viewer_->showInterrupted(label_->text(), false);
        break;
    case DesktopSession::Requested: {
        label_->setText(
            (session_.extended()
                 ? (control ? tr("%1 请求查看和切换显示器、使用键鼠。文件需逐次接收，剪贴板需双方开启。")
                            : tr("%1 请求查看桌面并切换显示器。"))
                 : (control ? tr("%1 请求查看主屏幕并使用键鼠。") : tr("%1 请求查看主屏幕。")))
                .arg(name));
        showDevicesWindow(*window());
        QApplication::alert(window());
        break;
    }
    case DesktopSession::Sharing:
        updateSound();
        if (control && session_.extended())
            addTransferBar();
        session_.updateHostDisplays(desktopDisplays());
        break;
    case DesktopSession::Viewing:
        label_->setText((control ? tr("正在控制 %1 的桌面。") : tr("正在查看 %1 的桌面。")).arg(name));
        openViewer();
        if (control && session_.extended())
            addTransferBar();
        if (!session_.extended())
            startPlayer();
        break;
    }
}

void DesktopControls::addTransferBar()
{
    auto &data = session_.dataLink();
    transfer_ = new TransferBar(data, viewer_ ? static_cast<QWidget *>(viewer_) : this,
                                viewer_ ? viewer_->sessionMenu() : nullptr);
    if (viewer_) {
        viewer_->addTools(transfer_);
        connect(viewer_, &DesktopViewer::fileDropped, transfer_, [&data](const QString &path) { data.offerFile(path); });
        auto updateDrops = [this] {
            auto &link = session_.dataLink();
            if (viewer_)
                viewer_->allowFileDrops(link.ready() && link.fileState() == SessionData::NoFile);
        };
        connect(&data, &SessionData::changed, transfer_, updateDrops);
        updateDrops();
    } else
        column_->addWidget(transfer_);
}

void DesktopControls::switchDisplay(bool formatOnly)
{
    retireMedia(formatOnly);
    if (session_.state() == DesktopSession::Sharing) {
        QString name;
        for (const auto &d : session_.displays())
            if (d.id == session_.displayId())
                name = d.name;
        label_->setText(
            tr("%1 正在%2 %3")
                .arg(session_.peerName(), session_.interactive() ? tr("控制") : tr("查看"), name));
        startCapture();
        if (session_.interactive() && !formatOnly) {
            inputSink_ = createInputSink(session_.displayId());
            input_ = std::make_unique<HostInput>(session_.peerOS(), localOS(), *inputSink_);
            focusTimer_->start();
            refreshInputFocus();
        }
    } else if (viewer_) {
        if (!formatOnly)
            viewer_->inputSurface()->changeDisplay();
        viewer_->setDisplays(session_.displays(), session_.displayId());
        viewer_->waitForPicture(formatOnly ? tr("正在调整画质…") : tr("正在切换显示器…"));
        startPlayer();
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
                if (ui->session_.quality() != VideoQuality::FullHD && gate->capturedStamp &&
                    quint32(stamp - *gate->capturedStamp) < 9000)
                    ui->settleMotion_.start();
                gate->capturedStamp = stamp;
                ui->session_.sendFrame(std::move(data), stamp, key);
            });
        },
        [gate](QString error) {
            Callbacks::post(gate, [gate, error](DesktopControls *ui) {
                QTimer::singleShot(280, ui, [ui, gate, error] {
                    if (ui->callbacks_ != gate)
                        return;
                    const auto displays = desktopDisplays();
                    if (ui->session_.extended() && displays != ui->session_.displays())
                        ui->session_.updateHostDisplays(displays, true);
                    else if (!ui->session_.reduceQuality())
                        ui->session_.stop(error);
                });
            });
        },
        session_.displayId(), session_.quality());
}

void DesktopControls::openViewer()
{
    const bool control = session_.interactive();
    if (viewer_) {
        viewer_->showDesktop(session_.richMedia());
        if (control)
            viewer_->focusVideo();
        return;
    }
    viewer_ = new DesktopViewer(this, control, session_.richMedia());
    connect(viewer_, &DesktopViewer::reconnectRequested, this, [this] {
        if (session_.state() == DesktopSession::Interrupted)
            if (auto peer = reconnectPeer())
                session_.view(*peer, session_.interactive());
    });
    connect(viewer_, &DesktopViewer::qualityRequested, &session_, &DesktopSession::chooseQuality);
    connect(viewer_, &DesktopViewer::soundRequested, &session_, &DesktopSession::enableSound);
    viewer_->setWindowTitle((control ? tr("%1 · 控制桌面") : tr("%1 · 只读桌面")).arg(session_.peerName()));
    connect(viewer_, &DesktopViewer::displayRequested, &session_, &DesktopSession::chooseDisplay);
    connect(viewer_, &DesktopViewer::viewportChanged, this, [this] {
        if (player_)
            player_->setViewport(viewer_->inputSurface()->viewport());
    });
    if (control) {
        auto *surface = viewer_->inputSurface();
        connect(surface, &InputWindow::input, this, [this](const InputEvent &e) { session_.sendInput(e); });
        connect(viewer_, &DesktopViewer::switchRequested, this, [this] {
            InputEvent event;
            event.kind = InputKind::SwitchApp;
            session_.sendInput(event);
        });
    }
    connect(viewer_, &QDialog::finished, this, [this] { session_.stop(); });
    viewer_->show();
    if (control)
        viewer_->focusVideo();
}

void DesktopControls::startPlayer()
{
    auto gate = callbacks_ = std::make_shared<Callbacks>(this);
    player_ = playDesktop(
        viewer_->videoView(),
        [gate](quint32 stamp) {
            gate->displayedStamp = stamp;
            ++gate->presented;
            if (gate->firstPicture.exchange(false))
                Callbacks::post(gate, [](DesktopControls *ui) {
                    ui->session_.picturePresented();
                    if (ui->viewer_) {
                        ui->viewer_->pictureShown();
                        if (ui->session_.interactive())
                            ui->viewer_->inputSurface()->presented();
                    }
                });
        },
        Callbacks::error(gate));
    player_->setViewport(viewer_->inputSurface()->viewport());
}
} // namespace wwmm
