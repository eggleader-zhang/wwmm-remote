#pragma once
#include "desktop_display.h"
#include "video_packets.h"
#include <QDialog>
class QComboBox;
class QCheckBox;
class QVBoxLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QScrollBar;
class QAction;

namespace wwmm {
class InputWindow;

class DesktopViewer final : public QDialog {
    Q_OBJECT
  public:
    explicit DesktopViewer(QWidget *parent = nullptr, bool interactive = false, bool rich = false);
    quintptr videoView() const;

    InputWindow *inputSurface() const { return surface_; }

    void focusVideo();
    void setStatus(const QString &text);
    void showQuality(VideoQuality limit, VideoQuality active);
    void showInterrupted(const QString &message, bool canRetry);
    void showDesktop(bool rich);
    void showSound(bool local, bool peer);
    void setDisplays(const QList<DesktopDisplay> &list, quint64 selected);
    void addTools(QWidget *tools);
    void allowFileDrops(bool allow);
  signals:
    void switchRequested();
    void displayRequested(quint64 id);
    void qualityRequested(wwmm::VideoQuality quality);
    void soundRequested(bool enable);
    void fileDropped(const QString &path);
    void reconnectRequested();
    void viewportChanged();

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    InputWindow *surface_;
    QWidget *video_;
    QWidget *picturePage_;
    QScrollBar *horizontal_, *vertical_;
    QAction *original_;
    void updateViewport();
    void applyViewport();
    void toggleFullScreen();
    QStackedWidget *pages_;
    QLabel *interrupted_;
    QPushButton *retry_, *switchApp_ = nullptr;
    QLabel *status_;
    QComboBox *displays_, *quality_ = nullptr;
    QCheckBox *sound_ = nullptr;
    QVBoxLayout *column_;
};
} // namespace wwmm
