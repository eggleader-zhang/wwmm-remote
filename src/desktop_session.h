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
#include <functional>
#include <optional>

namespace wwmm {
struct ConnectionStats {
    QString tlsVersion, cipherSuite;
    Endpoint peer{};
    std::optional<double> tcpConnectMs, rttMs, jitterMs, lossPercent;
    qint64 mediaBytesPerSecond = 0, videoBitsPerSecond = 0;
    VideoQuality quality = VideoQuality::FullHD;
    int framesPerSecond = 0;
    QSize frameSize;
    std::optional<DesktopOS> peerOS;
    bool audioEnabled = false;
    bool adaptiveReduced = false;
};

class DesktopSession final : public QObject {
    Q_OBJECT
  public:
    enum State { Idle, Connecting, Waiting, Requested, Sharing, Viewing, Interrupted };

    DesktopSession(LanProbe &probe, DeviceStore &store, std::function<bool(QString &)> allowInput,
                   QObject *parent = nullptr);
    ~DesktopSession() override;

    State state() const { return state_; }
    bool canInstallUpdate() const { return state_ == Idle && !control_ && data_.fileState() == SessionData::NoFile; }
    bool reserveUpdate() { return installing_ = canInstallUpdate(); }
    void releaseUpdate() { installing_ = false; }

    QString peerName() const;
    ConnectionStats connectionStats() const;
    void view(const NearbyDevice &peer, bool withInput = false);

    bool interactive() const { return interactive_; }

    bool extended() const { return extended_; }

    bool richMedia() const { return rich_; }

    VideoQuality quality() const { return quality_; }
    VideoQuality qualityLimit() const { return qualityLimit_; }

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

    bool usesMacKeyboard() const { return state_ == Viewing && macKeyboardProtocol_ && autoMacKeys_; }
    bool supportsRttProbe() const { return rttProtocol_; }
    void setAutoMacKeys(bool enabled);
    bool canSetRemoteCursor() const { return cursorProtocol_ && !cursorUnavailable_; }
    bool prefersRemoteCursor() const { return preferCursor_; }
    bool captureCursorVisible() const { return captureCursor_; }
    void setRemoteCursorVisible(bool visible);
    void reportCursorUnavailable();

    void sendInput(InputEvent event, bool useMacKeyboard = true);
    void reportFocus(InputFocus focus);
    void accept(bool rememberDevice = false);
    void stop(const QString &reason = {});
    void interrupt(const QString &reason);
    void sendFrame(QByteArray annexB, quint32 timestamp, bool keyframe);
    void refreshVideo();
  signals:
    void changed();
    void ended(bool targetUnreachable);
    void displaysChanged();
    void displaySwitched(bool formatOnly);
    void beforePaste();
    void qualityChanged();
    void soundChanged();
    void connectionStatsChanged();
    void notice(const QString &message);
    void frame(const QByteArray &annexB, quint32 timestamp);
    void keyframeNeeded();
    void bitrateChanged(int bitsPerSecond);
    void inputReceived(const wwmm::InputEvent &event);
    void focusReceived(wwmm::InputFocus focus);
    void macKeyboardChanged(bool enabled);
    void captureCursorChanged();
    void remoteCursorUnavailable();

  private:
    struct Network;
    void attach(QTcpSocket *socket, const LanAddress &link, QByteArray expected, bool withInput = false);
    bool prepareNetwork();
    bool startNetwork();
    bool displayMessage(const QByteArray &data);
    bool mediaMessage(const QByteArray &data);
    void appendMediaOptions(QByteArray &message) const;
    void probeRtt();
    void applyQuality(VideoQuality quality, bool keepEstimate = false);
    void syncSound();
    void attachSound();
    void startDisplay(quint64 id, bool formatOnly = false);
    void drainPaste();
    void handle(const QByteArray &data);
    void startSharing(bool rememberDevice);
    void setState(State state);
    void check();
    void finishSession(const QString &reason, bool transportLost);
    void receiveInput(const InputEvent &event);
    LanProbe &probe_;
    DeviceStore &store_;
    std::function<bool(QString &)> allowInput_;
    SessionData data_;
    TrustedChannel *control_ = nullptr;
    std::unique_ptr<Network> network_;
    State state_ = Idle;
    bool installing_ = false;
    LanAddress link_;
    Endpoint remote_;
    QByteArray selected_;
    QString peerName_;
    ConnectionStats stats_;
    bool host_ = false, received_ = false, rich_ = false, adaptive_ = false, retrying_ = false;
    bool usingDirectAccess_ = false;
    VideoQuality quality_ = VideoQuality::FullHD, qualityLimit_ = VideoQuality::FullHD;
    VideoQuality hardwareLimit_ = VideoQuality::UHD;
    AdaptiveResolution resolution_;
    quint32 localSound_ = 0, peerSound_ = 0;
    std::shared_ptr<AudioStream> sound_;
    bool interactive_ = false, extended_ = false, displayReady_ = false, needDisplayIdr_ = false;
    bool macKeyboardProtocol_ = false, autoMacKeys_ = true;
    bool cursorProtocol_ = false, cursorUnavailable_ = false;
    bool preferCursor_ = true, captureCursor_ = true;
    bool rttProtocol_ = false;
    QTimer rttTimer_;
    QElapsedTimer rttClock_;
    std::optional<quint32> pendingRttToken_;
    quint32 rttSequence_ = 0;
    QList<DesktopDisplay> displays_, announced_;
    quint64 display_ = 0;
    QString rememberOnPresent_;
    quint32 boundary_ = 0;
    bool listing_ = false;
    QList<InputEvent> afterPaste_;
    std::optional<InputEvent> imagePaste_;
    quint8 imagePasteHeldMods_ = 0;
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
