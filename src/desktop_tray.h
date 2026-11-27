#pragma once
#include <QMenu>
#include <QSystemTrayIcon>

namespace wwmm {
class DesktopSession;

void showDevicesWindow(QWidget &window);

class DesktopTray final : public QObject {
  public:
    explicit DesktopTray(QWidget &window);
    void watch(DesktopSession &session);

  private:
    QWidget &window_;
    QMenu menu_;
    QSystemTrayIcon icon_;
    QAction *stop_;
};
} // namespace wwmm
