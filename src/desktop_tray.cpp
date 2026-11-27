#include "desktop_tray.h"
#include "desktop_session.h"
#include "desktop_update.h"
#include <QApplication>

namespace wwmm {
void showDevicesWindow(QWidget &window)
{
    if (window.isMinimized())
        window.setWindowState(window.windowState() & ~Qt::WindowMinimized);
    window.show();
    window.raise();
    window.activateWindow();
}

DesktopTray::DesktopTray(QWidget &window) : QObject(&window), window_(window), icon_(this)
{
    qApp->setQuitOnLastWindowClosed(false);
    qApp->setWindowIcon(QIcon(QStringLiteral(":/icons/wwmm.png")));
#ifdef Q_OS_MACOS
    QIcon glyph(QStringLiteral(":/icons/menubar.png"));
    glyph.setIsMask(true);
    icon_.setIcon(glyph);
#else
    icon_.setIcon(qApp->windowIcon());
#endif
    icon_.setToolTip(qApp->applicationName());
    auto *open = menu_.addAction(tr("打开设备窗口"));
    connect(open, &QAction::triggered, this, [this] { showDevicesWindow(window_); });
    stop_ = menu_.addAction(tr("停止共享"));
    stop_->setVisible(false);
    menu_.addSeparator();
    update_ = new DesktopUpdate(menu_, window, this);
    auto *quit = menu_.addAction(tr("退出 WWMM Remote"));
    quit->setMenuRole(QAction::NoRole);
    connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit);
    icon_.setContextMenu(&menu_);
    connect(&icon_, &QSystemTrayIcon::messageClicked, open, &QAction::trigger);
#ifdef Q_OS_WIN
    connect(&icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            showDevicesWindow(window_);
    });
#endif
    icon_.show();
}

void DesktopTray::watch(DesktopSession &session)
{
    update_->watch(session);
    connect(stop_, &QAction::triggered, &session, [&session] { session.stop(); });
    connect(qApp, &QCoreApplication::aboutToQuit, &session, [&session] { session.stop(); });
    connect(&session, &DesktopSession::changed, this, [this, &session] {
        const bool sharing = session.state() == DesktopSession::Sharing;
        stop_->setVisible(sharing);
        if (!sharing) {
            icon_.setToolTip(qApp->applicationName());
            return;
        }
        const auto name = session.peerName();
        const auto status = (session.interactive() ? tr("%1 正在控制本机") : tr("%1 正在查看本机")).arg(name);
        icon_.setToolTip(status);
        stop_->setText(tr("停止与 %1 共享").arg(QString(name).replace('&', "&&")));
        if (!window_.isVisible() || window_.isMinimized())
            icon_.showMessage(qApp->applicationName(), status, QSystemTrayIcon::Information);
    });
}
} // namespace wwmm
