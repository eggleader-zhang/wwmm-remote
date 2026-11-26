#pragma once
#include "probe_message.h"
#include <QMap>
#include <QPointF>
#include <QSizeF>
#include <array>
#include <memory>
#include <optional>

namespace wwmm {
enum class InputFocus : quint8 { Raw, Text, Terminal, Office };
enum class InputKind : quint8 { KeyDown, KeyUp, Text, Move, ButtonDown, ButtonUp, Wheel, Reset, SwitchApp };
enum class MouseButton : quint8 { Left = 1, Right, Middle, Back, Forward };

struct ScreenPoint {
    quint16 x = 0, y = 0;
    bool operator==(const ScreenPoint &) const = default;
};

struct InputEvent {
    InputKind kind = InputKind::Reset;
    quint8 key = 0, letter = 0, mods = 0;
    MouseButton button = MouseButton::Left;
    ScreenPoint point;
    quint32 sequence = 0, fence = 0;
    qint16 dx = 0, dy = 0;
    bool pixels = false;
    QString text;
};

QByteArray packInput(const InputEvent &e);
std::optional<InputEvent> readInput(const QByteArray &message);
QList<QString> inputTextChunks(const QString &text);
std::optional<ScreenPoint> screenPoint(QPointF local, QSizeF view, QSize picture, bool clamp);
bool newerInput(quint32 a, quint32 b);
quint8 modifierBit(quint8 usage);
int nativeKeyCode(DesktopOS os, quint8 usage);
quint8 hidKey(DesktopOS os, int nativeCode);

class InputSink {
  public:
    virtual ~InputSink() = default;
    virtual bool key(quint8 usage, bool down, bool repeat) = 0;
    virtual quint8 letterKey(quint8 ascii) = 0;
    virtual bool text(const QString &value) = 0;
    virtual bool pointer(ScreenPoint point, MouseButton button, InputKind kind) = 0;
    virtual bool wheel(ScreenPoint point, qint16 x, qint16 y, bool pixels) = 0;
};

class HostInput {
  public:
    HostInput(DesktopOS source, DesktopOS target, InputSink &sink);
    ~HostInput();
    bool apply(const InputEvent &event);

    void setFocus(InputFocus focus) { focus_ = focus; }

    void release();

  private:
    struct Binding {
        quint8 key = 0, requiredMods = 0;
        std::array<quint8, 8> modifiers{};
        quint8 mask(quint8 held) const;
        void remap(quint8 sourceGroup, quint8 targetLeft);
    };

    Binding binding(const InputEvent &e);
    bool keyEvent(const InputEvent &e);
    bool sync(quint8 desired);
    quint8 usedModifiers() const;
    bool switchApp();
    DesktopOS source_, target_;
    InputSink &sink_;
    InputFocus focus_ = InputFocus::Raw;
    QMap<quint8, Binding> keys_;
    quint8 sourceMods_ = 0, postedMods_ = 0, unusedMods_ = 0, buttons_ = 0;
    ScreenPoint lastPoint_;
};
} // namespace wwmm
