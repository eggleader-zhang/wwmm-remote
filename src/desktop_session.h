#pragma once
#include "discovery.h"
#include "desktop_display.h"
#include "session_data.h"
#include "audio_stream.h"
#include "video_packets.h"
#include "desktop_input.h"
#include "trusted_channel.h"
#include <QElapsedTimer>
#include <QTimer>

namespace wwmm {
class DesktopSession final : public QObject {
    Q_OBJECT
  public:
    enum State { Idle, Connecting, Waiting, Requested, Sharing, Viewing };

    DesktopSession(LanProbe &probe, DeviceStore &store, QObject *parent = nullptr);
    ~DesktopSession() override;

    State state() const { return state_; }

    QString peerName() const;
    void view(const NearbyDevice &peer, bool withInput = false);

    bool interactive() const { return interactive_; }

    bool extended() const { return extended_; }

    bool richMedia() const { return rich_; }

    VideoQuality quality() const { return quality_; }

    void chooseQuality(VideoQuality quality);
    bool reduceQuality();

    bool localSound() const { return localSound_ != 0; }

    bool peerSound() const { return peerSound_ != 0; }

    std::shared_ptr<AudioStream> sound() const { return sound_; }

    void enableSound(bool enable);

    QByteArray peerKey() const { return selected_; }

    SessionData &dataLink() { return data_; }

    const QList<DesktopDisplay> &displays() const { return displays_; }

    quint64 displayId() const { return display_; }

    void updateHostDisplays(QList<DesktopDisplay> displays, bool restart = false);
    void chooseDisplay(quint64 id);
    void picturePresented();

    DesktopOS peerOS() const { return peerOS_; }

    void sendInput(InputEvent event);
    void reportFocus(InputFocus focus);
    void accept();
    void stop(const QString &reason = {});
    void sendFrame(QByteArray annexB, quint32 timestamp, bool keyframe);
    void refreshVideo();
  signals:
    void changed();
    void displaysChanged();
    void displaySwitched();
    void beforePaste();
    void qualityChanged();
    void soundChanged();
    void notice(const QString &message);
    void frame(const QByteArray &annexB, quint32 timestamp);
    void keyframeNeeded();
    void bitrateChanged(int bitsPerSecond);
    void inputReceived(const wwmm::InputEvent &event);
    void focusReceived(wwmm::InputFocus focus);

  private:
    struct Network;
    void attach(QTcpSocket *socket, const LanAddress &link, QByteArray expected, bool withInput = false);
    bool prepareNetwork();
    bool startNetwork();
    bool displayMessage(const QByteArray &data);
    bool mediaMessage(const QByteArray &data);
    void applyQuality(VideoQuality quality);
    void syncSound();
    void attachSound();
    void startDisplay(quint64 id);
    void drainPaste();
    void handle(const QByteArray &data);
    void setState(State state);
    void check();
    void receiveInput(const InputEvent &event);
    LanProbe &probe_;
    DeviceStore &store_;
    SessionData data_;
    TrustedChannel *control_ = nullptr;
    std::unique_ptr<Network> network_;
    State state_ = Idle;
    LanAddress link_;
    Endpoint remote_;
    QByteArray selected_;
    bool host_ = false, received_ = false, rich_ = false;
    VideoQuality quality_ = VideoQuality::FullHD;
    quint32 localSound_ = 0, peerSound_ = 0;
    std::shared_ptr<AudioStream> sound_;
    bool interactive_ = false, extended_ = false, displayReady_ = false, needDisplayIdr_ = false;
    QList<DesktopDisplay> displays_, announced_;
    quint64 display_ = 0;
    quint32 boundary_ = 0;
    bool listing_ = false;
    QList<InputEvent> afterPaste_;
    std::optional<InputEvent> imagePaste_;
    quint64 pasteRevision_ = 0;
    DesktopOS peerOS_ = DesktopOS::macOS;
    quint32 pointerSequence_ = 0, pointerFence_ = 0, lastPosition_ = 0;
    QTimer settlePointer_;
    std::optional<InputEvent> latestPosition_;
    quint64 epoch_ = 0;
    QTimer timer_, stillFrame_, pasteTimer_;
    std::optional<quint32> lastSent_, lastReceived_, wanted_;
    QElapsedTimer missingSince_;
    QElapsedTimer stateClock_, lastSeen_, keepalive_;
};
} // namespace wwmm
