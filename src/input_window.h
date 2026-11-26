#pragma once
#include "desktop_input.h"
#include <QHash>
#include <QTimer>
#include <QWindow>

namespace wwmm {
class InputWindow final : public QWindow {
    Q_OBJECT
  public:
    explicit InputWindow(bool interactive);

    void setPictureSize(QSize s)
    {
        if (!s.isEmpty())
            picture_ = s;
    }

    bool hasPictureSize() const { return !picture_.isEmpty(); }

    void presented();
    void setInputFocus(InputFocus kind);

    QObject *focusObject() const override { return const_cast<InputWindow *>(this); }

  signals:
    void input(const wwmm::InputEvent &event);
    void preeditChanged(const QString &text);
    void firstPictureShown();

  protected:
    bool event(QEvent *event) override;

  private:
    void resetInput();
    void cancelPreedit();
    void handleKey(QKeyEvent *event);
    void sendMove(ScreenPoint point);
    bool mouse(QMouseEvent *event);
    bool wheel(QWheelEvent *event);

    bool acceptsText() const { return interactive_ && ready_ && focused_ && focus_ == InputFocus::Text; }

    bool interactive_, ready_ = false, focused_ = false, composing_ = false, resetting_ = false;
    InputFocus focus_ = InputFocus::Raw;
    QSize picture_;
    quint8 mods_ = 0, buttons_ = 0;
    QHash<quint8, quint8> held_;
    QPointF caret_{8, 8};
    std::optional<ScreenPoint> pendingMove_;
    QTimer motion_;
};
} // namespace wwmm
