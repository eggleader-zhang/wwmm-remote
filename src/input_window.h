#pragma once
#include "desktop_input.h"
#include <QHash>
#include <QTimer>
#include <QWindow>

namespace wwmm {
class WindowsKeyboardGrab;
class InputWindow final : public QWindow {
    Q_OBJECT
  public:
    explicit InputWindow(bool interactive);
    ~InputWindow() override;

    void setPictureSize(QSize s)
    {
        if (!s.isEmpty() && picture_ != s) {
            picture_ = s;
            emit pictureSizeChanged();
        }
    }

    bool hasPictureSize() const { return !picture_.isEmpty(); }
    QSize pictureSize() const { return picture_; }
    VideoViewport viewport() const { return viewport_; }
    void setViewport(VideoViewport viewport) { viewport_ = viewport; }

    void presented();
    void changeDisplay();

    void allowFileDrops(bool allow) { fileDrops_ = allow; }
    void setInputFocus(InputFocus kind);
    void setMacKeyboard(bool enabled);
    bool keyboardGrabbed() const;

    QObject *focusObject() const override { return const_cast<InputWindow *>(this); }

  signals:
    void input(const wwmm::InputEvent &event);
    void preeditChanged(const QString &text);
    void firstPictureShown();
    void keyboardReleaseRequested();
    void keyboardGrabFailed();
    void fileDropped(const QString &path);
    void pictureSizeChanged();
    void pixelRatioChanged();

  protected:
    bool event(QEvent *event) override;

  private:
    void resetInput(bool notify = true);
    void updateKeyboardGrab();
    void cancelPreedit();
    void handleKey(QKeyEvent *event);
    void sendMove(ScreenPoint point);
    bool mouse(QMouseEvent *event);
    bool wheel(QWheelEvent *event);

    bool acceptsText() const { return interactive_ && ready_ && focused_ && focus_ == InputFocus::Text; }

    bool interactive_, ready_ = false, focused_ = false, composing_ = false, resetting_ = false;
    bool macKeyboard_ = false;
#ifdef Q_OS_WIN
    std::unique_ptr<WindowsKeyboardGrab> grab_;
    quint64 grabGeneration_ = 0;
#endif
    bool fileDrops_ = false;
    InputFocus focus_ = InputFocus::Raw;
    QSize picture_;
    VideoViewport viewport_;
    quint8 mods_ = 0, buttons_ = 0;
    QHash<quint8, quint8> held_;
    QPointF caret_{8, 8};
    std::optional<ScreenPoint> pendingMove_;
    QTimer motion_;
};
} // namespace wwmm
