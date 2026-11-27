#include "devices_panel.h"
#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <memory>

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
    std::unique_ptr<QWidget> window(wwmm::createDevicesPage(probe, store));
    window->setWindowTitle(QStringLiteral("WWMM Remote"));
    window->resize(500, 340);
    QObject::connect(&probe, &wwmm::LanProbe::receivingChanged, &app, [&](bool enabled) {
        settings.setValue(QStringLiteral("diagnostics/receive"), enabled);
    });
    window->show();
    return app.exec();
}
