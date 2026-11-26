#include "main_window.h"
#include "devices_panel.h"
#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("WWMM"));
    app.setOrganizationDomain(QStringLiteral("wwmm.dev"));
    app.setApplicationName(QStringLiteral("WWMM Remote"));
    QSettings settings;
    wwmm::LanProbe probe;
    probe.setReceiving(settings.value(QStringLiteral("diagnostics/receive"), true).toBool());
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    wwmm::DeviceStore store(directory, wwmm::systemVault(directory));
    QTabWidget window;
    window.setWindowTitle(QStringLiteral("WWMM Remote"));
    window.addTab(wwmm::createDevicesPage(probe, store), QStringLiteral("设备"));
    window.addTab(new wwmm::MainWindow(probe), QStringLiteral("连接检测"));
    window.resize(560, 480);
    QObject::connect(&probe, &wwmm::LanProbe::receivingChanged, &app, [&](bool enabled) {
        settings.setValue(QStringLiteral("diagnostics/receive"), enabled);
    });
    window.show();
    return app.exec();
}
