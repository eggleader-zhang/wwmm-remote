#pragma once
#include "pairing.h"
#include <QTimer>
#include <QWidget>

class QAction;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace wwmm
{
QWidget *createDevicesPage(LanProbe &probe, DeviceStore &store);
class DesktopSession;

class DevicesPanel final : public QWidget
{
    Q_OBJECT
  public:
    DevicesPanel(LanProbe &, Discovery &, Pairing &, DeviceStore &, QWidget *parent = nullptr);
    DesktopSession &desktopSession() const { return *desktop_; }

  private:
    void refreshList();
    void updateActions();
    void updateOffer();
    void requestDesktop(bool control);
    void showStatus(const QString &message);
    QByteArray selectedKey() const;
    QStringList recent_;
    QString recentSetting_;
    LanProbe &probe_;
    Discovery &discovery_;
    Pairing &pairing_;
    DeviceStore &store_;
    DesktopSession *desktop_;
    QListWidget *list_;
    QLabel *empty_, *issue_, *localCode_, *status_;
    QLineEdit *code_;
    QPushButton *offer_, *pair_, *primary_;
    QPushButton *more_;
    QAction *view_, *control_, *directAccess_, *repair_, *forget_;
    QTimer refresh_, countdown_;
    QByteArray actionKey_, repairKey_;
    bool primaryControl_ = false;
};
} // namespace wwmm
