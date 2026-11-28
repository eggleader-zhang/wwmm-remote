#pragma once
#include "desktop_display.h"
#include "video_packets.h"
#include <QByteArray>
#include <QDialog>
#include <QElapsedTimer>
#include <QStringList>
#include <QTimer>
#include <optional>
class QMenu;
class QVBoxLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QScrollBar;
class QTabBar;
class QAction;

namespace wwmm {
enum class RemoteShortcut { SwitchApp, LockScreen, Search, Overview, ShowDesktop };
enum class StatsCorner { TopLeft, TopRight, BottomLeft, BottomRight };
struct ConnectionStats;
class ControlCenterPopup;
class ElidedLabel;
class FullscreenHandle;
class InputWindow;
class StatsOverlay;

class DesktopViewer final : public QDialog {
    Q_OBJECT
  public:
    explicit DesktopViewer(QWidget *parent = nullptr, bool interactive = false, bool rich = false,
                           const QList<QAction *> &cursorActions = {}, QAction *macKeysAction = nullptr);
    ~DesktopViewer() override;
    quintptr videoView() const;

    InputWindow *inputSurface() const { return surface_; }

    void focusVideo();
    void setPeerName(const QString &name);
    void setPeerIdentity(const QByteArray &key);
    void setLocalCursorVisible(bool visible);
    void setStatus(const QString &text, int durationMs = 6000, bool showInFullscreen = false);
    void waitForPicture(const QString &message);
    void pictureShown();
    void setConnectionStats(const ConnectionStats &stats);
    StatsCorner statsCorner() const { return statsCorner_; }
    void setStatsCorner(StatsCorner corner);
    QMenu *sessionMenu() const { return menu_; }
    void insertSessionAction(QAction *action);
    void showQuality(VideoQuality limit, VideoQuality active);
    void showInterrupted(const QString &message, bool canRetry);
    void showDesktop(bool rich);
    void showSound(bool local, bool peer);
    void setDisplays(const QList<DesktopDisplay> &list, quint64 selected);
    void addTools(QWidget *tools);
    void allowFileDrops(bool allow);
  signals:
    void switchRequested();
    void remoteShortcutRequested(wwmm::RemoteShortcut shortcut);
    void displayRequested(quint64 id);
    void qualityRequested(wwmm::VideoQuality quality);
    void soundRequested(bool enable);
    void fileDropped(const QString &path);
    void filesDropped(const QStringList &paths);
    void reconnectRequested();
    void viewportChanged();

  protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;

  private:
    void updateViewport();
    void applyViewport();
    void captureWindowedGeometry();
    void saveViewState();
    void toggleFullScreen();
    void revealHeader();
    void scheduleHeaderHide();
    void updateFullscreenHandle();
    void requestDisplay(quint64 id);
    void cycleDisplay(int step);
    void updateTheme();
    void updateTabIcons();
    void updateStatus();
    void updateSessionTime();
    void updateNetworkSignal(const ConnectionStats &stats);
    void updateNetworkIcon();
    void updateStatsOverlay();

    InputWindow *surface_;
    QWidget *video_;
    QWidget *picturePage_;
    QScrollBar *horizontal_, *vertical_;
    QStackedWidget *pages_;
    QLabel *interrupted_;
    QPushButton *retry_;
    QWidget *header_ = nullptr;
    QPushButton *controlCenter_, *pinHeader_;
    FullscreenHandle *fullscreenHandle_;
    ControlCenterPopup *popup_ = nullptr;
    ElidedLabel *peerLabel_, *status_;
    QTabBar *displayTabs_;
    QLabel *shield_, *networkSignal_, *sessionTime_;
    StatsOverlay *statsOverlay_;
    QMenu *menu_, *displays_, *quality_;
    QAction *fit_, *original_, *fullScreen_, *sound_, *collaboration_, *monitoring_, *stats_;
    QMenu *shortcuts_ = nullptr;
    QList<DesktopDisplay> displayList_;
    int selectedTab_ = 0, signalBars_ = -1;
    StatsCorner statsCorner_ = StatsCorner::TopLeft;
    std::optional<bool> darkTheme_;
    std::optional<double> lastJitterMs_, lastLossPercent_;
    QByteArray peerKey_, windowedGeometry_;
    QString peerName_, waitingMessage_, notice_, preedit_;
    QElapsedTimer sessionClock_;
    QTimer sessionTimer_, noticeTimer_, headerTimer_;
    bool headerHovered_ = false, fullscreenNotice_ = false;
    QVBoxLayout *column_;
};
} // namespace wwmm
