#pragma once
#include "session_data.h"
#include <QElapsedTimer>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <optional>

class QLabel;
class QPushButton;
class QAction;
class QFileDialog;
class QMimeData;

namespace wwmm {
QStringList droppedFiles(const QMimeData *mime);
QString droppedFile(const QMimeData *mime);
QString uniqueDownloadPath(const QString &dir, const QString &fileName);

class ClipboardImages;
class DesktopViewer;
class TransferBar final : public QWidget {
    Q_OBJECT
  public:
    explicit TransferBar(SessionData &data, QWidget *parent, DesktopViewer *viewer = nullptr);
    static bool autoClipboardSync();
    static void setAutoClipboardSync(bool enabled);
    void pollClipboard();
    void sendFile(const QString &path);
    void sendFiles(const QStringList &paths);

  signals:
    void clipboardSynced(const QString &summary);

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

  private:
    void update();
    void updateTransferStatus();
    void chooseFile(bool saving);
    void sendNextFile();
    void acceptFile(const QString &path);
    void saveToDownloads();
    void showNotice(const QString &message);
    SessionData &data_;
    ClipboardImages *images_;
    QAction *clipboard_, *send_ = nullptr;
    QLabel *status_;
    QPushButton *file_, *cancel_, *download_, *reveal_, *close_;
    QFileDialog *dialog_ = nullptr;
    QTimer poll_, queueTimer_, progressTimer_, completionTimer_;
    QElapsedTimer transferClock_;
    QStringList sendQueue_;
    qsizetype batchTotal_ = 0, batchIndex_ = 0;
    SessionData::FileState lastFileState_ = SessionData::NoFile;
    QString receivePath_, receiveNotice_, completedPath_;
    quint64 revision_ = 0;
    bool watching_ = false, applying_ = false, saving_ = false, clipboardReady_ = false;
    std::optional<QString> previous_;
};
} // namespace wwmm
