#pragma once
#include "session_data.h"
#include <QWidget>
#include <optional>

class QLabel;
class QPushButton;
class QAction;
class QMenu;
class QFileDialog;
class QMimeData;

namespace wwmm {
QString droppedFile(const QMimeData *mime);

class ClipboardImages;
class TransferBar final : public QWidget {
  public:
    explicit TransferBar(SessionData &data, QWidget *parent, QMenu *sessionMenu = nullptr);
    void pollClipboard();

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

  private:
    void update();
    void chooseFile(bool saving);
    SessionData &data_;
    ClipboardImages *images_;
    QAction *clipboard_, *send_ = nullptr;
    QLabel *status_;
    QPushButton *file_, *cancel_;
    QFileDialog *dialog_ = nullptr;
    QTimer poll_;
    quint64 revision_ = 0;
    bool watching_ = false, applying_ = false, saving_ = false;
    std::optional<QString> previous_;
};
} // namespace wwmm
