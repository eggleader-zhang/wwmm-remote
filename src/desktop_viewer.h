#pragma once
#include "desktop_display.h"
#include <QDialog>
class QComboBox;
class QVBoxLayout;
class QLabel;

namespace wwmm {
class InputWindow;

class DesktopViewer final : public QDialog {
    Q_OBJECT
  public:
    explicit DesktopViewer(QWidget *parent = nullptr, bool interactive = false);
    quintptr videoView() const;

    InputWindow *inputSurface() const { return surface_; }

    void focusVideo();
    void setStatus(const QString &text);
    void setDisplays(const QList<DesktopDisplay> &list, quint64 selected);
    void addTools(QWidget *tools);
    void allowFileDrops(bool allow);
  signals:
    void switchRequested();
    void displayRequested(quint64 id);
    void fileDropped(const QString &path);

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

  private:
    InputWindow *surface_;
    QWidget *video_;
    QLabel *status_;
    QComboBox *displays_;
    QVBoxLayout *column_;
};
} // namespace wwmm
