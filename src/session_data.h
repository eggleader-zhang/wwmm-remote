#pragma once
#include "lan_address.h"
#include "trusted_channel.h"
#include <QFile>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <memory>

class QTcpServer;

namespace wwmm {
// A separate, session-bound TLS socket carries clipboard and file traffic in both directions.
class SessionData final : public QObject {
    Q_OBJECT
  public:
    explicit SessionData(DeviceStore &store, QObject *parent);
    ~SessionData() override;
    QByteArray listen(const LanAddress &link, QHostAddress peer, QByteArray key);
    void join(const LanAddress &link, QHostAddress peer, QByteArray key, const QByteArray &offer);
    void reset();

    bool ready() const { return bound_; }

    bool localClipboard() const { return localClip_; }

    bool clipboardEnabled() const { return bound_ && localClip_ && peerClip_; }

    bool clipboardPending() const { return pendingClip_ || receivingClip_; }

    void enableClipboard(bool enable);
    void copyText(const QString &text);

    void noteActivity() { activity_.start(); }

    static constexpr int maxTextBytes = 1024 * 1024;

    enum FileState { NoFile, Offering, Offered, Sending, Receiving };

    FileState fileState() const { return fileState_; }

    QString fileName() const { return name_; }

    qint64 fileSize() const { return total_; }

    qint64 progress() const { return progress_; }

    void offerFile(const QString &path);
    void acceptFile(const QString &path);
    void cancelFile();
  signals:
    void changed();
    void clipboardText(const QString &text);
    void clipboardSettled();
    void progressChanged();
    void notice(const QString &message);

  private:
    struct Disk;

    struct DiskResult {
        bool ok;
        QByteArray data;
        qint64 size = 0;
    };

    using Operation = std::function<DiskResult(Disk &)>;
    void disk(Operation work, std::function<void(DiskResult)> finish);
    void attach(QTcpSocket *socket, bool server);
    void handle(const QByteArray &frame);
    void handleFile(const QByteArray &frame);
    void tick();
    void clearFile();
    void clearClipboard();
    void fileError(const QString &reason);
    bool send(const QByteArray &frame);
    void failed();
    DeviceStore &store_;
    QTcpServer *server_ = nullptr;
    TrustedChannel *channel_ = nullptr;
    QTimer pump_, bindTimer_;
    QElapsedTimer activity_;
    LanAddress link_{};
    QHostAddress remote_;
    QByteArray peer_, token_;
    bool bound_ = false, host_ = false, localClip_ = false, peerClip_ = false;
    quint64 revision_ = 0, pendingClip_ = 0, receivingClip_ = 0;
    QByteArray writer_, outText_, inText_;
    int textOffset_ = 0, expectedText_ = 0;
    bool startText_ = false;
    FileState fileState_ = NoFile;
    QByteArray id_;
    QString name_;
    qint64 total_ = 0, progress_ = 0, awaiting_ = 0;
    bool diskBusy_ = false, ending_ = false;
    std::shared_ptr<Disk> file_;
    QObject *diskWorker_ = nullptr;
    QThread diskThread_;
};
} // namespace wwmm
