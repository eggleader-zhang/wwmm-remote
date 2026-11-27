#include "devices_panel.h"
#include "desktop_tray.h"
#include "release_version.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QSysInfo>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#ifdef Q_OS_MACOS
#include <QMenuBar>
#endif
#include <memory>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("WWMM"));
    app.setOrganizationDomain(QStringLiteral("wwmm.dev"));
    app.setApplicationName(QStringLiteral("WWMM Remote"));
    if (app.arguments().contains("--installation-info")) {
        QFile out;
        if (!out.open(stdout, QIODevice::WriteOnly) || QIcon(":/icons/wwmm.png").pixmap(32).isNull()) return 1;
        out.write(QJsonDocument(QJsonObject{{"Product","dev.wwmm.remote"},{"Version",WWMM_VERSION_STRING},
            {"Build",WWMM_BUILD_NUMBER},{"CPU",QSysInfo::buildCpuArchitecture()}}).toJson(QJsonDocument::Compact));
        return out.flush() ? 0 : 1;
    }
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const auto socketName = QStringLiteral("wwmm-remote-") + QString::fromLatin1(
        QCryptographicHash::hash(directory.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    QLockFile instanceLock(QDir::temp().filePath(socketName + ".lock"));
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock()) {
        if (instanceLock.error() == QLockFile::LockFailedError) {
            // The first launch may still be between acquiring the lock and starting its listener.
            for (int attempt = 0; attempt < 12; ++attempt) {
                QLocalSocket existing;
                existing.connectToServer(socketName);
                if (existing.waitForConnected(100))
                    return 0;
                QThread::msleep(50);
            }
        }
        QMessageBox::warning(nullptr, app.applicationName(),
                             QObject::tr("无法打开设备窗口。可从菜单栏或系统托盘打开已运行的应用。"));
        return 1;
    }
    QLocalServer launcher;
#ifdef Q_OS_WIN
    // The per-user installer detects this mutex and asks for a graceful tray exit.
    const auto runningMutex = CreateMutexW(nullptr, FALSE, L"Local\\WWMMRemote-Desktop");
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [runningMutex] {
        if (runningMutex) CloseHandle(runningMutex);
    });
#endif
    launcher.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(socketName);
    if (!launcher.listen(socketName)) {
        QMessageBox::warning(nullptr, app.applicationName(), launcher.errorString());
        return 1;
    }
#ifdef Q_OS_MACOS
    // Qt supplies a native Command-Q item even in a menu-bar utility.
    QMenuBar appMenu;
    auto *quitRole = appMenu.addMenu(app.applicationName())->addAction(QObject::tr("退出"));
    quitRole->setMenuRole(QAction::QuitRole);
    quitRole->setEnabled(false);
#endif
    QSettings settings;
    wwmm::LanProbe probe;
    probe.setReceiving(settings.value(QStringLiteral("diagnostics/receive"), true).toBool());
    wwmm::DeviceStore store(directory, wwmm::systemVault(directory));
    std::unique_ptr<QWidget> window(wwmm::createDevicesPage(probe, store));
    window->setWindowTitle(QStringLiteral("WWMM Remote"));
    window->resize(500, 340);
    QObject::connect(&launcher, &QLocalServer::newConnection, window.get(), [&] {
        while (auto *socket = launcher.nextPendingConnection())
            socket->deleteLater();
        wwmm::openDevicesWindow(*window);
    });
    QObject::connect(&probe, &wwmm::LanProbe::receivingChanged, &app, [&](bool enabled) {
        settings.setValue(QStringLiteral("diagnostics/receive"), enabled);
    });
    window->show();
    return app.exec();
}
