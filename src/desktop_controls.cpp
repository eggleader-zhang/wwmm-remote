#include "desktop_controls.h"
#include "desktop_viewer.h"
#include "desktop_tray.h"
#include "input_window.h"
#include "transfer_bar.h"
#include "video_packets.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QMutex>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <atomic>
#include <cmath>
#ifdef Q_OS_MACOS
#include <CoreGraphics/CGDirectDisplay.h>
#include <CoreGraphics/CGDisplayConfiguration.h>
#elif defined(Q_OS_WIN)
#include <QtGui/qscreen_platform.h>
#endif

namespace wwmm {
namespace {
constexpr quint8 leftControl = 224, leftGui = 227, tab = 43;

bool isWindowsTaskViewKey(const InputEvent &event)
{
    return event.kind == InputKind::KeyDown && event.key == tab && event.mods == modifierBit(leftGui);
}

QScreen *screenForDisplay(quint64 id)
{
    if (id) {
#ifdef Q_OS_MACOS
        const auto display = CGDirectDisplayID(id);
        if (CGDisplayIsActive(display)) {
            const auto bounds = CGDisplayBounds(display);
            if (auto *screen = QGuiApplication::screenAt({qRound(CGRectGetMidX(bounds)), qRound(CGRectGetMidY(bounds))}))
                return screen;
        }
#elif defined(Q_OS_WIN)
        const auto monitor = reinterpret_cast<HMONITOR>(quintptr(id));
        for (auto *screen : QGuiApplication::screens()) {
            const auto *native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
            if (native && native->handle() == monitor)
                return screen;
        }
#endif
    }
    return QGuiApplication::primaryScreen();
}
} // 匿名命名空间

class HostPrivacyBar final : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(HostPrivacyBar)
  public:
    explicit HostPrivacyBar(DesktopSession &session)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                              Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint),
          interactive_(session.interactive())
    {
        setObjectName("hostPrivacyBar");
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_QuitOnClose, false);
#ifdef Q_OS_MACOS
        setAttribute(Qt::WA_MacAlwaysShowToolWindow);
#endif
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeHorCursor);
        setStyleSheet(QStringLiteral(R"(
            QLabel { color: #F1F4FA; font-weight: 600; }
            QPushButton { color: #E4EBF5; background: #3A4556; border: none;
                border-radius: 9px; min-height: 20px; padding: 1px 7px; }
            QPushButton:hover { background: #4C5A70; }
            QPushButton:checked { color: #A9E8DB; background: #30554F; }
            QPushButton#hostPrivacyStop { color: #FFD1D6; background: #643941; }
            QPushButton#hostPrivacyStop:hover { background: #86454F; }
            QPushButton#hostPrivacyCollapse { color: #E4EBF5; background: transparent; }
            QPushButton#hostPrivacyCollapse:hover { background: #4C5A70; }
        )"));

        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(25, 4, 5, 4);
        row->setSpacing(5);
        row->setSizeConstraint(QLayout::SetFixedSize);
        const auto name = session.peerName();
        message_ = (interactive_ ? tr("正在被 %1 控制") : tr("正在向 %1 共享")).arg(name);
        setAccessibleName(message_);
        setToolTip(message_);
        label_ = new QLabel(this);
        label_->setObjectName("hostPrivacyStatus");
        label_->setTextFormat(Qt::PlainText);
        label_->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(label_);

        sound_ = new QPushButton(this);
        sound_->setObjectName("hostPrivacySound");
        sound_->setCheckable(true);
        stop_ = new QPushButton(tr("停止"), this);
        stop_->setObjectName("hostPrivacyStop");
        collapse_ = new QPushButton(this);
        collapse_->setObjectName("hostPrivacyCollapse");
        collapse_->setCheckable(true);
        for (auto *button : {sound_, stop_, collapse_}) {
            button->setFocusPolicy(Qt::NoFocus);
            button->setAutoDefault(false);
            button->setCursor(Qt::PointingHandCursor);
            row->addWidget(button);
        }
        connect(sound_, &QPushButton::clicked, &session, &DesktopSession::enableSound);
        connect(stop_, &QPushButton::clicked, &session, [&session] { session.stop(); });
        connect(collapse_, &QPushButton::toggled, this, &HostPrivacyBar::updateContents);
        updateContents();
        pulse_.start();
        connect(&pulseTimer_, &QTimer::timeout, this, [this] { update(7, 0, 14, height()); });
        pulseTimer_.start(75);
    }

    void setSound(bool available, bool local, bool peer)
    {
        rich_ = available;
        const QSignalBlocker block(sound_);
        sound_->setChecked(local);
        sound_->setText(local ? tr("声音已开") : tr("开启声音"));
        sound_->setToolTip(local && !peer ? tr("等待对方收听")
                           : !local && peer ? tr("对方请求系统声音") : tr("共享系统声音"));
        updateContents();
    }

    void placeOnScreen(QScreen *screen)
    {
        if (screen_ != screen) {
            screen_ = screen;
            offset_ = 0;
            if (screen)
                setScreen(screen);
        }
        layout()->activate();
        if (screen_) {
            const auto area = screen_->availableGeometry();
            placeHorizontally(area.x() + (area.width() - width()) / 2 + offset_);
        }
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor(255, 255, 255, 58));
        painter.setBrush(QColor(30, 34, 42, 238));
        const qreal radius = height() / 2.0;
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        QColor dot(interactive_ ? "#FFAC62" : "#7FD7A5");
        dot.setAlphaF(float(0.8 + 0.2 * std::cos(double(pulse_.elapsed()) * 0.003)));
        painter.setPen(Qt::NoPen);
        painter.setBrush(dot);
        painter.drawEllipse(QPointF(14, radius), 4.5, 4.5);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            dragOrigin_ = event->globalPosition().toPoint().x() - x();
            event->accept();
        } else
            QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragOrigin_ && event->buttons().testFlag(Qt::LeftButton)) {
            placeHorizontally(event->globalPosition().toPoint().x() - *dragOrigin_);
            event->accept();
        } else
            QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && dragOrigin_) {
            dragOrigin_.reset();
            event->accept();
        } else
            QWidget::mouseReleaseEvent(event);
    }

  private:
    void placeHorizontally(int requested)
    {
        if (!screen_)
            return;
        const auto area = screen_->availableGeometry();
        const int left = qBound(area.left(), requested, qMax(area.left(), area.right() + 1 - width()));
        offset_ = left - (area.x() + (area.width() - width()) / 2);
        move(left, area.y() + 8);
    }

    void updateContents()
    {
        const bool collapsed = collapse_->isChecked();
        const auto status = collapsed ? (interactive_ ? tr("控制中") : tr("共享中")) : message_;
        label_->setText(label_->fontMetrics().elidedText(status, Qt::ElideRight, 260));
        sound_->setVisible(!collapsed && rich_);
        stop_->setVisible(!collapsed);
        collapse_->setText(collapsed ? QStringLiteral("▾") : QStringLiteral("▴"));
        collapse_->setToolTip(collapsed ? tr("展开共享状态") : tr("折叠共享状态"));
        collapse_->setAccessibleName(collapse_->toolTip());
        placeOnScreen(screen_);
    }

    const bool interactive_;
    bool rich_ = false;
    QString message_;
    QLabel *label_;
    QPushButton *sound_, *stop_, *collapse_;
    QPointer<QScreen> screen_;
    QElapsedTimer pulse_;
    QTimer pulseTimer_;
    int offset_ = 0;
    std::optional<int> dragOrigin_;
};

QList<InputEvent> remoteShortcutEvents(RemoteShortcut shortcut, DesktopOS target)
{
    const bool mac = target == DesktopOS::macOS;
    QList<quint8> keys;
    switch (shortcut) {
    case RemoteShortcut::SwitchApp: {
        InputEvent event;
        event.kind = InputKind::SwitchApp;
        return {event};
    }
    case RemoteShortcut::LockScreen:
        keys = mac ? QList<quint8>{leftControl, leftGui, 20} : QList<quint8>{leftGui, 15};
        break;
    case RemoteShortcut::Search:
        keys = {leftGui, quint8(mac ? 44 : 22)};
        break;
    case RemoteShortcut::Overview:
        keys = mac ? QList<quint8>{leftControl, 82} : QList<quint8>{leftGui, tab};
        break;
    case RemoteShortcut::ShowDesktop:
        keys = mac ? QList<quint8>{68} : QList<quint8>{leftGui, 7};
        break;
    }
    QList<InputEvent> events;
    quint8 mods = 0;
    for (const quint8 key : keys) {
        mods |= modifierBit(key);
        InputEvent event;
        event.kind = InputKind::KeyDown;
        event.key = key;
        event.mods = mods;
        events.append(event);
    }
    for (auto key = keys.crbegin(); key != keys.crend(); ++key) {
        mods &= ~modifierBit(*key);
        InputEvent event;
        event.kind = InputKind::KeyUp;
        event.key = *key;
        event.mods = mods;
        events.append(event);
    }
    return events;
}

bool applyRemoteInput(HostInput &input, InputSink &sink, const InputEvent &event, bool windowsTaskView)
{
    if (!windowsTaskView || !isWindowsTaskViewKey(event))
        return input.apply(event);
    // v17 的 Mac 查看端使用 SwitchApp 传递日常 Cmd+Tab，Win+Tab 按 Windows 布局注入。
    input.release();
    HostInput native(DesktopOS::windows, DesktopOS::windows, sink);
    for (const auto &key : remoteShortcutEvents(RemoteShortcut::Overview, DesktopOS::windows))
        if (!native.apply(key))
            return false;
    return true;
}

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

DesktopControls::DesktopControls(DesktopSession &session, Discovery &discovery, QMenu *settings, QWidget *parent,
                                 QAction *macKeysAction)
    : QWidget(parent), session_(session), discovery_(discovery), macKeysAction_(macKeysAction)
{
    auto cursorAction = [this, settings](const QString &label, const QString &key) {
        auto *action = new QAction(label, this);
        action->setCheckable(true);
        action->setChecked(QSettings().value(key, true).toBool());
        settings->addAction(action);
        connect(action, &QAction::triggered, this, [key](bool visible) { QSettings().setValue(key, visible); });
        return action;
    };
    localCursor_ = cursorAction(tr("是否显示本机鼠标"), QStringLiteral("viewer/showLocalCursor"));
    remoteCursor_ = cursorAction(tr("是否显示远程鼠标"), QStringLiteral("viewer/showRemoteCursor"));
    localCursor_->setToolTip(tr("在远控画面中显示这台电脑的指针"));
    session.setRemoteCursorVisible(remoteCursor_->isChecked());
    updateCursorAction();
    connect(localCursor_, &QAction::triggered, this, [this](bool visible) {
        if (viewer_)
            viewer_->setLocalCursorVisible(visible);
    });
    connect(remoteCursor_, &QAction::triggered, &session, &DesktopSession::setRemoteCursorVisible);
    connect(&session, &DesktopSession::remoteCursorUnavailable, this, [this] {
        updateCursorAction();
        if (viewer_)
            viewer_->setStatus(tr("共享端无法隐藏系统指针，已恢复显示远程鼠标。"));
    });
    connect(&session, &DesktopSession::captureCursorChanged, this, [this] {
        if (capture_) {
            retireMedia(true);
            startCapture();
        }
    });
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
    connect(&session, &DesktopSession::macKeyboardChanged, this, &DesktopControls::showMacKeyboardHint);
    connect(&session, &DesktopSession::qualityChanged, this, [this] {
        if (viewer_)
            viewer_->showQuality(session_.qualityLimit(), session_.quality());
    });
    connect(&session, &DesktopSession::connectionStatsChanged, this, &DesktopControls::updateConnectionStats);
    connect(&session, &DesktopSession::notice, this, [this](const QString &message) {
        if (viewer_)
            viewer_->setStatus(message);
    });
    rememberAccess_ = new QPushButton(tr("允许并记住"), this);
    rememberAccess_->setObjectName("rememberAccess");
    rememberAccess_->setToolTip(tr("允许这台已配对设备以后直接查看和控制本机"));
    row->addWidget(rememberAccess_);
    row->addWidget(yes_);
    row->addWidget(stop_);
    focusTimer_ = new QTimer(this);
    focusTimer_->setInterval(200);
    connect(focusTimer_, &QTimer::timeout, this, &DesktopControls::refreshInputFocus);
    connect(rememberAccess_, &QPushButton::clicked, this, [this] { session_.accept(true); });
    connect(yes_, &QPushButton::clicked, this, [this] { session_.accept(); });
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
        connect(screen, &QScreen::availableGeometryChanged, this, topologyChanged);
        connect(screen, &QScreen::physicalDotsPerInchChanged, this, topologyChanged);
        connect(screen, &QScreen::orientationChanged, this, topologyChanged);
        topologyChanged();
    };
    for (auto *screen : QGuiApplication::screens())
        watchScreen(screen);
    connect(qApp, &QGuiApplication::screenAdded, this, watchScreen);
    connect(qApp, &QGuiApplication::screenRemoved, this, topologyChanged);
    connect(qApp, &QGuiApplication::primaryScreenChanged, this, topologyChanged);
    connect(&topology_, &QTimer::timeout, this, &DesktopControls::updateHostDisplays);
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
        const auto elapsed = qMax<qint64>(1, frameClock_.restart());
        frameRate_ = qRound(double(count) * 1000 / double(elapsed));
        updateConnectionStats();
        if (!count && !outstanding)
            frameTimer_.stop();
    });
    connect(&session, &DesktopSession::frame, this, [this](const QByteArray &frame, quint32 time) {
        if (player_) {
            if (!callbacks_->receivedStamp || !viewer_->inputSurface()->hasPictureSize())
                viewer_->inputSurface()->setPictureSize(h264PictureSize(frame));
            callbacks_->receivedStamp = time;
            if (!frameTimer_.isActive()) {
                frameClock_.start();
                frameTimer_.start();
            }
            player_->push(frame, time);
        }
    });
    connect(&session, &DesktopSession::inputReceived, this, [this](const InputEvent &event) {
        if (!input_)
            return;
        const bool windowsTaskView = session_.supportsRttProbe() && localOS() == DesktopOS::windows &&
                                     session_.peerOS() == DesktopOS::macOS;
        if (!applyRemoteInput(*input_, *inputSink_, event, windowsTaskView)) {
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
    updateState();
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
    frameRate_ = 0;
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
    if (viewer_) {
        viewer_->showSound(local, peer);
        updateConnectionStats();
    }
    {
        QSignalBlocker block(soundCheck_);
        soundCheck_->setChecked(local);
        soundCheck_->setText(!local && peer ? tr("对方请求系统声音")
                                            : (local && !peer ? tr("等待对方收听") : tr("共享系统声音")));
    }
    if (privacyBar_)
        privacyBar_->setSound(session_.richMedia(), local, peer);
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

void DesktopControls::updateConnectionStats()
{
    if (!viewer_ || session_.state() != DesktopSession::Viewing)
        return;
    auto stats = session_.connectionStats();
    stats.framesPerSecond = frameRate_;
    stats.frameSize = viewer_->inputSurface()->pictureSize();
    viewer_->setConnectionStats(stats);
}

void DesktopControls::endMedia(bool keepViewer)
{
    closeSound();
    retireMedia();
    topology_.stop();
    viewerHasPicture_ = false;
    if (!keepViewer)
        macHintShown_ = false;
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
    const auto text = online ? tr("已在局域网中找到 %1。")
                             : tr("正在等待 %1 回到局域网…");
    viewer_->showInterrupted(text.arg(session_.peerName()), online);
}

void DesktopControls::updateCursorAction()
{
    const bool available = session_.state() != DesktopSession::Viewing || session_.canSetRemoteCursor();
    remoteCursor_->setChecked(!available || session_.prefersRemoteCursor());
    remoteCursor_->setEnabled(available);
    remoteCursor_->setToolTip(available ? tr("在远程画面中显示共享端的系统指针")
                                        : tr("当前共享端不支持隐藏鼠标指针"));
}

void DesktopControls::updateState()
{
    const auto s = session_.state();
    const bool control = session_.interactive();
    if (s != DesktopSession::Sharing && privacyBar_) {
        privacyBar_->hide();
        privacyBar_.release()->deleteLater();
    }
    updateCursorAction();
    setVisible(s != DesktopSession::Idle);
    yes_->setVisible(s == DesktopSession::Requested);
    rememberAccess_->setVisible(s == DesktopSession::Requested && control);
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
        if (!privacyBar_)
            privacyBar_ = std::make_unique<HostPrivacyBar>(session_);
        updateSound();
        if (control && session_.extended())
            addTransferBar();
        updateHostDisplays();
        if (privacyBar_)
            privacyBar_->show();
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
    if (transfer_)
        return;
    auto &data = session_.dataLink();
    transfer_ = new TransferBar(data, viewer_ ? static_cast<QWidget *>(viewer_) : this, viewer_);
    if (viewer_) {
        viewer_->addTools(transfer_);
        connect(viewer_, &DesktopViewer::filesDropped, transfer_, &TransferBar::sendFiles);
        connect(transfer_, &TransferBar::clipboardSynced, viewer_,
                [viewer = viewer_](const QString &summary) { viewer->setStatus(summary, 2000); });
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

void DesktopControls::updateHostDisplays()
{
    // 离屏环境仍显示共享状态，跳过系统录屏与权限请求。
    if (QGuiApplication::platformName() != "offscreen")
        session_.updateHostDisplays(desktopDisplays());
    if (privacyBar_)
        privacyBar_->placeOnScreen(screenForDisplay(session_.displayId()));
}

void DesktopControls::switchDisplay(bool formatOnly)
{
    retireMedia(formatOnly);
    if (session_.state() == DesktopSession::Sharing) {
        if (privacyBar_)
            privacyBar_->placeOnScreen(screenForDisplay(session_.displayId()));
        if (QGuiApplication::platformName() == "offscreen")
            return;
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
        updateConnectionStats();
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
        session_.displayId(), session_.quality(), session_.captureCursorVisible(), [gate] {
            Callbacks::post(gate, [](DesktopControls *ui) { ui->session_.reportCursorUnavailable(); });
        });
}

void DesktopControls::openViewer()
{
    const bool control = session_.interactive();
    const auto name = session_.peerName();
    const auto title = (control ? tr("%1 · 控制桌面") : tr("%1 · 只读桌面")).arg(name);
    if (viewer_) {
        viewer_->setPeerIdentity(session_.peerKey());
        viewer_->inputSurface()->setMacKeyboard(session_.usesMacKeyboard());
        viewer_->showDesktop(session_.richMedia());
        viewer_->setPeerName(name);
        viewer_->setWindowTitle(title);
        updateConnectionStats();
        if (control)
            viewer_->focusVideo();
        return;
    }
    const bool mapsMacKeys = control && localOS() == DesktopOS::windows && session_.peerOS() == DesktopOS::macOS;
    viewer_ = new DesktopViewer(this, control, session_.richMedia(), {localCursor_, remoteCursor_},
                                mapsMacKeys ? macKeysAction_ : nullptr);
    viewer_->setPeerIdentity(session_.peerKey());
    viewer_->setLocalCursorVisible(localCursor_->isChecked());
    connect(viewer_, &DesktopViewer::reconnectRequested, this, [this] {
        if (session_.state() == DesktopSession::Interrupted)
            if (auto peer = reconnectPeer())
                session_.view(*peer, session_.interactive());
    });
    connect(viewer_, &DesktopViewer::qualityRequested, &session_, &DesktopSession::chooseQuality);
    connect(viewer_, &DesktopViewer::soundRequested, &session_, &DesktopSession::enableSound);
    viewer_->setPeerName(name);
    viewer_->setWindowTitle(title);
    connect(viewer_, &DesktopViewer::displayRequested, &session_, &DesktopSession::chooseDisplay);
    connect(viewer_, &DesktopViewer::viewportChanged, this, [this] {
        if (player_)
            player_->setViewport(viewer_->inputSurface()->viewport());
    });
    if (control) {
        auto *surface = viewer_->inputSurface();
        surface->setMacKeyboard(session_.usesMacKeyboard());
        connect(&session_, &DesktopSession::macKeyboardChanged, surface, &InputWindow::setMacKeyboard);
        connect(surface, &InputWindow::input, this, [this](const InputEvent &event) {
            if (session_.supportsRttProbe() && localOS() == DesktopOS::macOS &&
                session_.peerOS() == DesktopOS::windows && isWindowsTaskViewKey(event)) {
                session_.sendInput(InputEvent{});
                sendRemoteShortcut(RemoteShortcut::SwitchApp);
            } else {
                session_.sendInput(event);
            }
        });
        connect(viewer_, &DesktopViewer::remoteShortcutRequested, this, &DesktopControls::sendRemoteShortcut);
    }
    connect(viewer_, &QDialog::finished, this, [this] { session_.stop(); });
    updateConnectionStats();
    viewer_->show();
    if (control)
        viewer_->focusVideo();
}

void DesktopControls::showMacKeyboardHint()
{
    if (viewer_ && viewerHasPicture_ && !macHintShown_ && session_.usesMacKeyboard()) {
        macHintShown_ = true;
        viewer_->setStatus(tr("已启用 Mac 键位映射（Alt=⌘，Win=⌥），按 Ctrl+Alt+Shift+F12 释放键盘"), 5000, true);
    }
}

void DesktopControls::sendRemoteShortcut(RemoteShortcut shortcut)
{
    const auto target = session_.peerOS();
    if (shortcut == RemoteShortcut::Overview && localOS() == DesktopOS::macOS &&
        target == DesktopOS::windows && !session_.supportsRttProbe()) {
        viewer_->setStatus(tr("共享端版本暂不支持发送任务视图快捷键。"));
        return;
    }
    if (shortcut != RemoteShortcut::SwitchApp)
        session_.sendInput(InputEvent{});
    // 组合键已使用目标平台的 HID 键码，跳过 Windows 查看 macOS 时的 Alt 与 Win 映射。
    for (const auto &event : remoteShortcutEvents(shortcut, target))
        session_.sendInput(event, false);
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
                    if (ui->viewer_ && ui->session_.state() == DesktopSession::Viewing) {
                        ui->viewer_->pictureShown();
                        if (ui->session_.interactive())
                            ui->viewer_->inputSurface()->presented();
                        ui->viewerHasPicture_ = true;
                        ui->showMacKeyboardHint();
                    }
                });
        },
        Callbacks::error(gate));
    player_->setViewport(viewer_->inputSurface()->viewport());
}
} // namespace wwmm
