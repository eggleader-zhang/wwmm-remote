#pragma once
#include "pairing.h"
#include <QTimer>
#include <QWidget>

class QAction;
class QCheckBox;
class QFrame;
class QHideEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
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
    void openLastDevice();

  protected:
    void hideEvent(QHideEvent *event) override;

  private:
    void refreshList();
    void updateActions();
    void updateOffer();
    void connectSelectedDevice();
    void requestDesktop(bool control);
    void rememberTarget(const QByteArray &key, bool control);
    void tryAutoConnect(bool timerDue = false);
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
    QFrame *pairCodeCard_;
    QLabel *empty_, *issue_, *localCode_, *pairCodeDetails_, *status_;
    QLineEdit *code_;
    QCheckBox *pairAccess_;
    QProgressBar *pairCountdownBar_;
    QPushButton *offer_, *copyCode_, *pair_, *primary_;
    QPushButton *more_;
    QAction *view_, *control_, *directAccess_, *repair_, *forget_, *autoConnect_;
    QTimer refresh_, countdown_, copyFeedback_, autoDial_;
    QByteArray actionKey_, repairKey_, pendingPeer_;
    int retryDelayMs_ = 0;
    bool primaryControl_ = false, pendingControl_ = false, asleep_ = false;
};
} // namespace wwmm
