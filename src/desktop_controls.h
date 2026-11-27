#pragma once
#include "desktop_session.h"
#include "native_desktop.h"
#include "native_input.h"
#include "native_audio.h"
#include <QWidget>
class QLabel;
class QCheckBox;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace wwmm {
class DesktopViewer;
class TransferBar;

class DesktopControls final : public QWidget {
  public:
    DesktopControls(DesktopSession &session, Discovery &discovery, QWidget *parent);
    ~DesktopControls() override;

  private:
    struct Callbacks;
    void updateState();
    void startCapture();
    void openViewer();
    void switchDisplay(bool formatOnly);
    void startPlayer();
    void retireMedia(bool keepInput = false);
    void updateSound();
    void closeSound();
    void addTransferBar();
    void endMedia(bool keepViewer = false);
    std::optional<NearbyDevice> reconnectPeer() const;
    void updateReconnect();
    void refreshInputFocus();
    DesktopSession &session_;
    Discovery &discovery_;
    QLabel *label_;
    QCheckBox *soundCheck_;
    std::shared_ptr<Callbacks> soundCallbacks_;
    std::shared_ptr<AudioStream> audioStream_;
    std::unique_ptr<NativeAudio> audio_;
    QVBoxLayout *column_;
    TransferBar *transfer_ = nullptr;
    QPushButton *yes_, *stop_;
    DesktopViewer *viewer_ = nullptr;
    std::shared_ptr<Callbacks> callbacks_;
    std::unique_ptr<DesktopCapture> capture_;
    std::unique_ptr<DesktopPlayer> player_;
    std::unique_ptr<InputSink> inputSink_;
    std::unique_ptr<HostInput> input_;
    QTimer *focusTimer_;
    QTimer settleMotion_, topology_, frameTimer_;
    QElapsedTimer lastPolish_;
    bool dragging_ = false;
    std::optional<InputFocus> lastFocus_;
    bool focusPending_ = false;
};
} // namespace wwmm
