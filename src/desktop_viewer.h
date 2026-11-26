#pragma once
#include <QDialog>
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
  signals:
    void switchRequested();

  private:
    InputWindow *surface_;
    QWidget *video_;
    QLabel *status_;
};
} // namespace wwmm
