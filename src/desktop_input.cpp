#include "desktop_input.h"
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace wwmm {
namespace {
constexpr quint8 ctrl = 0x11, shift = 0x22, alt = 0x44, gui = 0x88;
constexpr quint8 backspaceKey = 42, homeKey = 74, deleteKey = 76, endKey = 77;
constexpr quint8 rightKey = 79, leftKey = 80, downKey = 81, upKey = 82;

quint8 sided(quint8 original, quint8 left)
{
    return original & 0xf0 ? quint8(left << 4) : left;
}

template <class T> void append(QByteArray &b, T n)
{
    char wire[sizeof(T)];
    qToBigEndian(n, wire);
    b.append(wire, sizeof(T));
}
} // namespace

bool newerInput(quint32 a, quint32 b)
{
    return qint32(a - b) > 0;
}

std::optional<ScreenPoint> screenPoint(QPointF local, QSize view, QSize picture, bool clamp, VideoViewport viewport)
{
    const auto [source, dest] = viewport.place(picture, view);
    if (dest.isEmpty())
        return {};
    const auto point = local - dest.topLeft();
    if (!clamp && (point.x() < 0 || point.y() < 0 || point.x() >= dest.width() || point.y() >= dest.height()))
        return {};
    auto axis = [](double p, int span, int start, int length, int full) {
        const auto fraction = std::clamp(p / std::max(1, span - 1), 0., 1.);
        return quint16(qRound((start + fraction * (length - 1)) * 65535. / std::max(1, full - 1)));
    };
    return ScreenPoint{axis(point.x(), dest.width(), source.x(), source.width(), picture.width()),
                       axis(point.y(), dest.height(), source.y(), source.height(), picture.height())};
}

QByteArray packInput(const InputEvent &e)
{
    QByteArray b("I", 1);
    b.append(char(e.kind));
    switch (e.kind) {
    case InputKind::KeyDown:
    case InputKind::KeyUp:
        b.append(char(e.key)).append(char(e.letter)).append(char(e.mods));
        break;
    case InputKind::Text:
        b.append(e.text.toUtf8());
        break;
    case InputKind::Move:
        append(b, e.fence);
        append(b, e.sequence);
        append(b, e.point.x);
        append(b, e.point.y);
        break;
    case InputKind::ButtonDown:
    case InputKind::ButtonUp:
    case InputKind::Wheel:
        append(b, e.sequence);
        append(b, e.point.x);
        append(b, e.point.y);
        b.append(char(e.mods));
        if (e.kind == InputKind::Wheel) {
            b.append(char(e.pixels));
            append(b, e.dx);
            append(b, e.dy);
        } else
            b.append(char(e.button));
        break;
    case InputKind::Reset:
        append(b, e.sequence);
        break;
    case InputKind::SwitchApp:
        break;
    }
    return b;
}

std::optional<InputEvent> readInput(const QByteArray &b)
{
    if (b.size() < 2 || b.size() > 255 || b[0] != 'I')
        return {};
    const char *p = b.constData();
    InputEvent e;
    e.kind = InputKind(quint8(b[1]));
    switch (e.kind) {
    case InputKind::KeyDown:
    case InputKind::KeyUp:
        if (b.size() != 5)
            return {};
        e.key = quint8(b[2]);
        e.letter = quint8(b[3]);
        e.mods = quint8(b[4]);
        if ((e.letter && (e.letter < 'A' || e.letter > 'Z')) ||
            (nativeKeyCode(DesktopOS::macOS, e.key) < 0 && nativeKeyCode(DesktopOS::windows, e.key) < 0))
            return {};
        break;
    case InputKind::Text: {
        const auto utf = QByteArrayView(b).sliced(2);
        if (utf.isEmpty() || !utf.isValidUtf8() || utf.contains(char(0)))
            return {};
        e.text = QString::fromUtf8(utf);
        break;
    }
    case InputKind::Move:
        if (b.size() != 14)
            return {};
        e.fence = qFromBigEndian<quint32>(p + 2);
        e.sequence = qFromBigEndian<quint32>(p + 6);
        e.point = {qFromBigEndian<quint16>(p + 10), qFromBigEndian<quint16>(p + 12)};
        break;
    case InputKind::ButtonDown:
    case InputKind::ButtonUp:
    case InputKind::Wheel:
        if (b.size() != (e.kind == InputKind::Wheel ? 16 : 12))
            return {};
        e.sequence = qFromBigEndian<quint32>(p + 2);
        e.point = {qFromBigEndian<quint16>(p + 6), qFromBigEndian<quint16>(p + 8)};
        e.mods = quint8(b[10]);
        if (e.kind == InputKind::Wheel) {
            if (quint8(b[11]) > 1)
                return {};
            e.pixels = b[11] != 0;
            e.dx = qFromBigEndian<qint16>(p + 12);
            e.dy = qFromBigEndian<qint16>(p + 14);
        } else {
            if (quint8(b[11]) < 1 || quint8(b[11]) > 5)
                return {};
            e.button = MouseButton(quint8(b[11]));
        }
        break;
    case InputKind::Reset:
        if (b.size() != 6)
            return {};
        e.sequence = qFromBigEndian<quint32>(p + 2);
        break;
    case InputKind::SwitchApp:
        if (b.size() != 2)
            return {};
        break;
    default:
        return {};
    }
    return e;
}

QList<QString> inputTextChunks(const QString &s)
{
    QList<QString> out;
    // Sixty UTF-16 units take at most 180 UTF-8 bytes; keep surrogate pairs together.
    for (qsizetype i = 0; i < s.size();) {
        qsizetype n = std::min<qsizetype>(60, s.size() - i);
        if (i + n < s.size() && s[i + n - 1].isHighSurrogate())
            --n;
        auto part = s.mid(i, n);
        part.remove(QChar::Null);
        if (!part.isEmpty())
            out.append(std::move(part));
        i += n;
    }
    return out;
}

HostInput::HostInput(DesktopOS source, DesktopOS target, InputSink &sink)
    : source_(source), target_(target), sink_(sink)
{
}

HostInput::~HostInput()
{
    release();
}

quint8 HostInput::Binding::mask(quint8 held) const
{
    quint8 out = requiredMods;
    for (int i = 0; i < 8; i++)
        if (held & (1u << i))
            out |= modifiers[i];
    return out;
}

void HostInput::Binding::remap(quint8 sourceGroup, quint8 targetLeft)
{
    for (auto &bit : modifiers)
        if (bit & sourceGroup)
            bit = sided(bit, targetLeft);
}

HostInput::Binding HostInput::binding(const InputEvent &e)
{
    Binding b;
    b.key = e.key;
    for (int i = 0; i < 8; i++)
        if (e.mods & (1u << i))
            b.modifiers[i] = quint8(1u << i);
    if (source_ == target_)
        return b;
    const bool mac = source_ == DesktopOS::macOS, terminal = focus_ == InputFocus::Terminal;
    const quint8 primary = mac ? gui : ctrl;
    const quint8 base = e.mods & ~shift;
    auto only = [base](quint8 group) { return base && !(base & ~group); };
    if (e.key == 43 && only(mac ? gui : alt)) {
        b.remap(mac ? gui : alt, mac ? 4 : 8);
        return b;
    }
    if (focus_ == InputFocus::Raw)
        return b;
    if (focus_ == InputFocus::Text) {
        const bool horizontal = e.key == leftKey || e.key == rightKey;
        if ((horizontal || e.key == backspaceKey || e.key == deleteKey) && only(mac ? alt : ctrl)) {
            b.remap(mac ? alt : ctrl, mac ? 1 : 4);
            return b;
        }
        if (mac && only(gui) && (horizontal || e.key == upKey || e.key == downKey)) {
            b.key = (e.key == leftKey || e.key == upKey) ? homeKey : endKey;
            b.remap(gui, horizontal ? 0 : 1);
            return b;
        }
        if (!mac && (e.key == homeKey || e.key == endKey) && (!base || only(ctrl))) {
            if (base) {
                b.key = e.key == homeKey ? upKey : downKey;
                b.remap(ctrl, 8);
            } else {
                b.key = e.key == homeKey ? leftKey : rightKey;
                b.requiredMods = 8;
            }
            return b;
        }
    }
    if (!(e.mods & primary) || (e.mods & ~(primary | shift)))
        return b;
    auto letter = e.letter;
    if (!letter || !QByteArrayView("CVXZAFSYBIUKONPWLT").contains(char(letter)))
        return b;
    const bool shifted = e.mods & shift;
    if (terminal && (letter != 'C' && letter != 'V'))
        return b;
    if (terminal && !mac && !shifted)
        return b; // Ctrl+C is the terminal's interrupt.
    if (terminal && mac && shifted)
        return b;
    const bool redo = !terminal && ((mac && letter == 'Z' && shifted) || (!mac && letter == 'Y' && !shifted));
    if (redo)
        letter = mac ? 'Y' : 'Z';
    const auto logical = sink_.letterKey(letter);
    b.key = logical ? logical : quint8(4 + letter - 'A');
    for (int i = 0; i < 8; i++) {
        const auto bit = quint8(1u << i);
        if (b.modifiers[i] & primary) {
            b.modifiers[i] = sided(bit, mac ? 1 : 8);
            if ((terminal && mac) || (redo && !mac))
                b.modifiers[i] |= 2;
        } else if ((terminal || (redo && mac)) && (bit & shift))
            b.modifiers[i] = 0;
    }
    return b;
}

quint8 HostInput::usedModifiers() const
{
    quint8 mask = buttons_ ? sourceMods_ : 0;
    for (const auto &b : keys_)
        mask |= b.mask(sourceMods_);
    return mask;
}

bool HostInput::sync(quint8 desired)
{
    for (bool press : {false, true})
        for (int i = 0; i < 8; i++) {
            quint8 bit = quint8(1u << i);
            if (bool(desired & bit) == press && bool(postedMods_ & bit) != press) {
                if (!sink_.key(quint8(224 + i), press, false))
                    return false;
                postedMods_ ^= bit;
            }
        }
    return true;
}

bool HostInput::keyEvent(const InputEvent &e)
{
    const bool down = e.kind == InputKind::KeyDown;
    const auto bit = modifierBit(e.key);
    sourceMods_ = e.mods;
    if (bit) {
        if (down) {
            if (!(usedModifiers() & bit))
                unusedMods_ |= bit;
        } else {
            const bool solo = unusedMods_ & bit;
            unusedMods_ &= ~bit;
            if (solo && !sync(usedModifiers() | bit))
                return false;
        }
        return sync(usedModifiers());
    }
    auto it = keys_.find(e.key);
    if (!down) {
        if (it == keys_.end())
            return true; // A locally composed key can have a key-up only.
        const auto k = it->key;
        bool shared = false;
        for (auto other = keys_.cbegin(); other != keys_.cend(); ++other)
            if (other.key() != e.key && other->key == k) {
                shared = true;
                break;
            }
        if (!shared && !sink_.key(k, false, false))
            return false;
        keys_.erase(it);
        return sync(usedModifiers());
    }
    unusedMods_ &= ~sourceMods_;
    const bool repeat = it != keys_.end();
    if (!repeat) {
        auto b = binding(e);
        if (nativeKeyCode(target_, b.key) < 0)
            return true;
        it = keys_.insert(e.key, b);
    }
    return sync(usedModifiers()) && sink_.key(it->key, true, repeat);
}

bool HostInput::switchApp()
{
    const quint8 m = target_ == DesktopOS::macOS ? 8 : 4;
    if (!sync(m))
        return false;
    if (!sink_.key(43, true, false))
        return false;
    bool ok = sink_.key(43, false, false);
    if (!ok)
        sink_.key(43, false, false);
    return sync(usedModifiers()) && ok;
}

bool HostInput::apply(const InputEvent &e)
{
    if (e.kind == InputKind::Reset) {
        release();
        return true;
    }
    if (e.kind == InputKind::KeyDown || e.kind == InputKind::KeyUp)
        return keyEvent(e);
    if (e.kind == InputKind::SwitchApp)
        return switchApp();
    if (e.kind == InputKind::Text) {
        unusedMods_ = 0;
        return focus_ != InputFocus::Text || (sync(0) && sink_.text(e.text) && sync(usedModifiers()));
    }
    lastPoint_ = e.point;
    if (e.kind == InputKind::Move)
        return sink_.pointer(e.point, MouseButton::Left, e.kind);
    sourceMods_ = e.mods;
    unusedMods_ &= ~sourceMods_;
    if (!sync(sourceMods_ | usedModifiers()))
        return false;
    if (e.kind == InputKind::Wheel)
        return sink_.wheel(e.point, e.dx, e.dy, e.pixels) && sync(usedModifiers());
    const quint8 bit = quint8(1u << (quint8(e.button) - 1));
    const bool down = e.kind == InputKind::ButtonDown;
    if (bool(buttons_ & bit) == down)
        return sync(usedModifiers());
    if (!sink_.pointer(e.point, e.button, e.kind))
        return false;
    buttons_ ^= bit;
    return sync(usedModifiers());
}

void HostInput::release()
{
    for (const auto &b : keys_)
        sink_.key(b.key, false, false);
    keys_.clear();
    for (int i = 0; i < 5; i++)
        if (buttons_ & (1u << i))
            sink_.pointer(lastPoint_, MouseButton(i + 1), InputKind::ButtonUp);
    for (int i = 0; i < 8; i++)
        if (postedMods_ & (1u << i))
            sink_.key(quint8(224 + i), false, false);
    buttons_ = sourceMods_ = postedMods_ = unusedMods_ = 0;
}
} // namespace wwmm
