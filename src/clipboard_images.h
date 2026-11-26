#pragma once
#include "session_data.h"
#include <QImage>
#include <optional>
class QMimeData;

namespace wwmm {
class ClipboardImages final : public QObject {
  public:
    using Install = std::function<bool(quint64, const QImage &)>;
    ClipboardImages(SessionData &data, Install install, QObject *parent);
    void copy(const QMimeData &mime);

  private:
    struct Job {
        quint64 revision;
        QByteArray png;
        QImage image;
    };

    void encode();
    void decode();
    SessionData &data_;
    Install install_;
    std::optional<Job> nextEncode_, nextDecode_;
    bool encoding_ = false, decoding_ = false;
};
} // namespace wwmm
