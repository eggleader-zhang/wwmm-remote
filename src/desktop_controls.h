#pragma once
#include "desktop_session.h"
#include "native_desktop.h"
#include "native_input.h"
#include <QWidget>
class QLabel;
class QPushButton;
class QTimer;

namespace wwmm {
class DesktopViewer;

class DesktopControls final : public QWidget {
  public:
    DesktopControls(DesktopSession &session, QWidget *parent);
    ~DesktopControls() override;

  private:
    struct Callbacks;
    void updateState();
    void startCapture();
    void openViewer();
    void endMedia();
    void refreshInputFocus();
    DesktopSession &session_;
    QLabel *label_;
    QPushButton *yes_, *stop_;
    DesktopViewer *viewer_ = nullptr;
    std::shared_ptr<Callbacks> callbacks_;
    std::unique_ptr<DesktopCapture> capture_;
    std::unique_ptr<DesktopPlayer> player_;
    std::unique_ptr<InputSink> inputSink_;
    std::unique_ptr<HostInput> input_;
    QTimer *focusTimer_;
    std::optional<InputFocus> lastFocus_;
    bool focusPending_ = false;
};
} // namespace wwmm
