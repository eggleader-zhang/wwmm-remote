#pragma once
#include "desktop_display.h"
#include "video_packets.h"
#include <QDialog>
class QComboBox;
class QCheckBox;
class QVBoxLayout;
class QLabel;

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
    void showQuality(VideoQuality quality);
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

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

  private:
    InputWindow *surface_;
    QWidget *video_;
    QLabel *status_;
    QComboBox *displays_, *quality_ = nullptr;
    QCheckBox *sound_ = nullptr;
    QVBoxLayout *column_;
};
} // namespace wwmm
