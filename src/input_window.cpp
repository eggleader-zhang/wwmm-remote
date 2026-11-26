#include "input_window.h"
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

namespace wwmm {
namespace {
quint8 fallbackKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return quint8(key - Qt::Key_A + 4);
    if (key >= Qt::Key_1 && key <= Qt::Key_9)
        return quint8(key - Qt::Key_1 + 30);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return quint8(key - Qt::Key_F1 + 58);
    switch (key) {
    case Qt::Key_0:
        return 39;
    case Qt::Key_Return:
        return 40;
    case Qt::Key_Enter:
        return 88;
    case Qt::Key_Escape:
        return 41;
    case Qt::Key_Backspace:
        return 42;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return 43;
    case Qt::Key_Space:
        return 44;
    case Qt::Key_Delete:
        return 76;
    case Qt::Key_Insert:
        return 73;
    case Qt::Key_Home:
        return 74;
    case Qt::Key_End:
        return 77;
    case Qt::Key_PageUp:
        return 75;
    case Qt::Key_PageDown:
        return 78;
    case Qt::Key_Right:
        return 79;
    case Qt::Key_Left:
        return 80;
    case Qt::Key_Down:
        return 81;
    case Qt::Key_Up:
        return 82;
    case Qt::Key_Shift:
        return 225;
    case Qt::Key_Alt:
        return 226;
    case Qt::Key_AltGr:
        return 230;
    case Qt::Key_Control:
        return localOS() == DesktopOS::macOS ? 227 : 224;
    case Qt::Key_Meta:
        return localOS() == DesktopOS::macOS ? 224 : 227;
    default:
        return 0;
    }
}

quint8 physicalKey(const QKeyEvent &e)
{
#ifdef Q_OS_MACOS
    // kVK_ANSI_A is zero even when the active layout puts another letter there.
    if (e.timestamp() || e.nativeVirtualKey() || e.key() == Qt::Key_A || e.nativeScanCode() ||
        e.nativeModifiers())
        return hidKey(DesktopOS::macOS, int(e.nativeVirtualKey()));
#else
    if (e.nativeScanCode()) {
        const auto native = e.nativeScanCode();
        int scan = int(native & 0xff);
        if ((native & 0xff00) == 0xe000 || (native & 0x100) || (e.nativeModifiers() & 0x01000000))
            scan |= 0x100;
        if (e.nativeVirtualKey() == 0x13)
            return 72; // VK_PAUSE
        if (e.nativeVirtualKey() == 0xa5 || e.key() == Qt::Key_AltGr)
            scan = 0x138;
        return hidKey(DesktopOS::windows, scan);
    }
#endif
    return fallbackKey(e.key());
}

quint8 currentModifiers(Qt::KeyboardModifiers qt, quint8 previous, quint32 native = 0)
{
    auto group = [&](bool present, quint8 bits, quint8 fallback) -> quint8 {
        return present ? ((previous & bits) ? (previous & bits) : fallback) : 0;
    };
#ifdef Q_OS_MACOS
    constexpr quint32 sides[] = {1, 2, 0x20, 8, 0x2000, 4, 0x40, 0x10};
    quint8 actual = 0;
    for (int i = 0; i < 8; i++)
        if (native & sides[i])
            actual |= quint8(1u << i);
    if (actual)
        previous = actual;
    const auto ctrl = Qt::MetaModifier, gui = Qt::ControlModifier;
#else
    Q_UNUSED(native);
    const auto ctrl = Qt::ControlModifier, gui = Qt::MetaModifier;
#endif
    return group(qt.testFlag(ctrl), 0x11, 1) | group(qt.testFlag(Qt::ShiftModifier), 0x22, 2) |
           group(qt.testFlag(Qt::AltModifier) || qt.testFlag(Qt::GroupSwitchModifier), 0x44, 4) |
           group(qt.testFlag(gui), 0x88, 8);
}

std::optional<MouseButton> buttonFor(Qt::MouseButton b)
{
    switch (b) {
    case Qt::LeftButton:
        return MouseButton::Left;
    case Qt::RightButton:
        return MouseButton::Right;
    case Qt::MiddleButton:
        return MouseButton::Middle;
    case Qt::BackButton:
        return MouseButton::Back;
    case Qt::ForwardButton:
        return MouseButton::Forward;
    default:
        return {};
    }
}

bool printable(QStringView text)
{
    if (text.isEmpty())
        return false;
    for (QChar c : text)
        if (c.unicode() < 32 || c.unicode() == 127 || (c.unicode() >= 0xf700 && c.unicode() <= 0xf8ff))
            return false;
    return true;
}
} // namespace

InputWindow::InputWindow(bool interactive) : interactive_(interactive)
{
    motion_.setSingleShot(true);
    motion_.setTimerType(Qt::PreciseTimer);
    motion_.setInterval(4);
    connect(&motion_, &QTimer::timeout, this, [this] {
        if (pendingMove_) {
            auto p = *pendingMove_;
            pendingMove_.reset();
            sendMove(p);
        }
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive && interactive_)
            resetInput();
    });
}

void InputWindow::presented()
{
    ready_ = true;
    if (focused_)
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    emit firstPictureShown();
}

void InputWindow::cancelPreedit()
{
    resetting_ = true;
    if (QGuiApplication::focusObject() == this)
        QGuiApplication::inputMethod()->reset();
    resetting_ = false;
    if (composing_)
        emit preeditChanged({});
    composing_ = false;
}

void InputWindow::setInputFocus(InputFocus kind)
{
    if (kind == focus_)
        return;
    cancelPreedit();
    focus_ = kind;
    if (QGuiApplication::focusObject() == this)
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
}

void InputWindow::resetInput()
{
    motion_.stop();
    pendingMove_.reset();
    held_.clear();
    mods_ = buttons_ = 0;
    cancelPreedit();
    if (ready_) {
        InputEvent e;
        e.kind = InputKind::Reset;
        emit input(e);
    }
}

void InputWindow::handleKey(QKeyEvent *event)
{
    const bool down = event->type() == QEvent::KeyPress;
    if (!down && event->isAutoRepeat())
        return;
    const auto code = physicalKey(*event);
    const auto bit = modifierBit(code);
    InputEvent e;
    if (bit) {
        if (event->isAutoRepeat())
            return;
        if (down)
            mods_ |= bit;
        else
            mods_ &= ~bit;
    } else {
        mods_ = currentModifiers(event->modifiers(), mods_, event->nativeModifiers());
        if (!down) {
            if (!held_.contains(code))
                return;
            e.key = held_.take(code);
        } else if (composing_)
            return;
        else {
            bool allowsLetters = !(mods_ & (0x11 | 0x88));
#ifdef Q_OS_WIN
            const bool altGr = (mods_ & 0x40) || event->modifiers().testFlag(Qt::GroupSwitchModifier);
            allowsLetters = (allowsLetters && !(mods_ & 0x44)) || (altGr && !(mods_ & 0x88));
#endif
            if (acceptsText() && allowsLetters && printable(event->text()) && !held_.contains(code)) {
                e.kind = InputKind::Text;
                e.text = event->text();
                emit input(e);
                return;
            }
            if (acceptsText() && allowsLetters && event->key() >= Qt::Key_Dead_Grave &&
                event->key() <= Qt::Key_Dead_Longsolidusoverlay)
                return;
            if (!code)
                return;
            e.key = held_.value(code, code);
            if (event->key() == Qt::Key_Delete && code == 42)
                e.key = 76;
            held_[code] = e.key;
        }
    }
    if (bit)
        e.key = code;
    e.kind = down ? InputKind::KeyDown : InputKind::KeyUp;
    e.mods = mods_;
    if (event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
        e.letter = quint8(event->key());
    emit input(e);
}

void InputWindow::sendMove(ScreenPoint point)
{
    InputEvent e;
    e.kind = InputKind::Move;
    e.point = point;
    emit input(e);
    motion_.start();
}

bool InputWindow::mouse(QMouseEvent *m)
{
    bool move = m->type() == QEvent::MouseMove;
    const bool down = m->type() == QEvent::MouseButtonPress || m->type() == QEvent::MouseButtonDblClick;
    auto p = screenPoint(m->position(), size(), picture_, buttons_ != 0);
    if (!p)
        return true;
    if (!move && down)
        requestActivate();
    if (move) {
        if (!focused_)
            return true;
        if (motion_.isActive())
            pendingMove_ = *p;
        else
            sendMove(*p);
        return true;
    }
    auto b = buttonFor(m->button());
    if (!b)
        return true;
    const auto bit = quint8(1u << (int(*b) - 1));
    if (bool(buttons_ & bit) == down)
        return true;
    pendingMove_.reset();
    motion_.stop();
    buttons_ ^= bit;
    caret_ = m->position();
    if (acceptsText())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
    InputEvent e;
    e.kind = down ? InputKind::ButtonDown : InputKind::ButtonUp;
    e.button = *b;
    e.point = *p;
    e.mods = currentModifiers(m->modifiers(), mods_);
    emit input(e);
    return true;
}

bool InputWindow::wheel(QWheelEvent *w)
{
    if (!focused_)
        return true;
    auto p = screenPoint(w->position(), size(), picture_, buttons_ != 0);
    if (!p)
        return true;
    InputEvent e;
    e.kind = InputKind::Wheel;
    e.pixels = !w->pixelDelta().isNull();
    const auto delta = e.pixels ? w->pixelDelta() : w->angleDelta();
    if (delta.isNull())
        return true;
    e.dx = qint16(qBound(-32767, delta.x(), 32767));
    e.dy = qint16(qBound(-32767, delta.y(), 32767));
    e.point = *p;
    e.mods = currentModifiers(w->modifiers(), mods_);
    pendingMove_.reset();
    motion_.stop();
    emit input(e);
    return true;
}

bool InputWindow::event(QEvent *ev)
{
    if (!interactive_)
        return QWindow::event(ev);
    if (ev->type() == QEvent::InputMethodQuery) {
        auto *query = static_cast<QInputMethodQueryEvent *>(ev);
        query->setValue(Qt::ImEnabled, acceptsText());
        query->setValue(Qt::ImHints, int(Qt::ImhNone));
        const QRectF cursor(caret_, QSizeF(1, 18));
        query->setValue(Qt::ImCursorRectangle, cursor);
        query->setValue(Qt::ImAnchorRectangle, cursor);
        query->setValue(Qt::ImSurroundingText, QString{});
        query->setValue(Qt::ImCurrentSelection, QString{});
        query->setValue(Qt::ImAnchorPosition, 0);
        query->setValue(Qt::ImCursorPosition, 0);
        ev->accept();
        return true;
    }
    if (ev->type() == QEvent::FocusIn) {
        focused_ = true;
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    }
    if (ev->type() == QEvent::FocusOut || ev->type() == QEvent::Hide) {
        focused_ = false;
        resetInput();
    }
    if (!ready_)
        return QWindow::event(ev);
    switch (ev->type()) {
    case QEvent::ShortcutOverride:
        if (!focused_)
            return QWindow::event(ev);
        ev->accept();
        return true;
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
        if (!focused_)
            return QWindow::event(ev);
        handleKey(static_cast<QKeyEvent *>(ev));
        ev->accept();
        return true;
    case QEvent::InputMethod: {
        auto *ime = static_cast<QInputMethodEvent *>(ev);
        if (acceptsText() && !resetting_) {
            composing_ = !ime->preeditString().isEmpty();
            emit preeditChanged(ime->preeditString());
            if (!ime->commitString().isEmpty()) {
                InputEvent e;
                e.kind = InputKind::Text;
                e.text = ime->commitString();
                emit input(e);
            }
        }
        ev->accept();
        return true;
    }
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
        return mouse(static_cast<QMouseEvent *>(ev));
    case QEvent::Wheel:
        return wheel(static_cast<QWheelEvent *>(ev));
    default:
        return QWindow::event(ev);
    }
}
} // namespace wwmm
