#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#include "native_input.h"
#include <algorithm>
#include <cmath>

namespace wwmm {
namespace {
CGEventFlags flagsFor(quint8 mods)
{
    CGEventFlags f = 0;
    if (mods & 0x11)
        f |= kCGEventFlagMaskControl;
    if (mods & 0x22)
        f |= kCGEventFlagMaskShift;
    if (mods & 0x44)
        f |= kCGEventFlagMaskAlternate;
    if (mods & 0x88)
        f |= kCGEventFlagMaskCommand;
    constexpr CGEventFlags sides[] = {1, 2, 0x20, 8, 0x2000, 4, 0x40, 0x10};
    for (int i = 0; i < 8; i++)
        if (mods & (1u << i))
            f |= sides[i];
    return f;
}

CGPoint location(ScreenPoint p, CGDirectDisplayID display)
{
    const auto bounds = CGDisplayBounds(display);
    return {bounds.origin.x + p.x * std::max(0., bounds.size.width - 1) / 65535.,
            bounds.origin.y + p.y * std::max(0., bounds.size.height - 1) / 65535.};
}

class MacInput final : public InputSink {
    CGEventSourceRef source_ = CGEventSourceCreate(kCGEventSourceStatePrivate);
    CGDirectDisplayID display_;
    quint8 mods_ = 0, buttons_ = 0;

    struct Click {
        CGPoint point{};
        double time = 0;
        int count = 0;
    };

    std::array<Click, 5> clicks_;

    bool post(CGEventRef event, CGEventFlags flags)
    {
        if (!event)
            return false;
        CGEventSetFlags(event, flags);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        return true;
    }

  public:
    explicit MacInput(CGDirectDisplayID display) : display_(display)
    {
        if (source_)
            CGEventSourceSetLocalEventsSuppressionInterval(source_, 0);
    }

    ~MacInput() override
    {
        if (source_)
            CFRelease(source_);
    }

    bool key(quint8 usage, bool down, bool repeat) override
    {
        int code = nativeKeyCode(DesktopOS::macOS, usage);
        if (code < 0)
            return true;
        auto event = CGEventCreateKeyboardEvent(source_, CGKeyCode(code), down);
        if (!event)
            return false;
        if (auto bit = modifierBit(usage)) {
            if (down)
                mods_ |= bit;
            else
                mods_ &= ~bit;
            CGEventSetType(event, kCGEventFlagsChanged);
        }
        CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, repeat);
        return post(event, flagsFor(mods_));
    }

    quint8 letterKey(quint8 ascii) override
    {
        auto input = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
        if (!input)
            return 0;
        auto data =
            static_cast<CFDataRef>(TISGetInputSourceProperty(input, kTISPropertyUnicodeKeyLayoutData));
        quint8 found = 0;
        if (data) {
            auto layout = reinterpret_cast<const UCKeyboardLayout *>(CFDataGetBytePtr(data));
            for (quint16 code = 0; code < 128 && !found; code++) {
                UInt32 state = 0;
                UniChar value[4]{};
                UniCharCount count = 0;
                if (UCKeyTranslate(layout, code, kUCKeyActionDown, 0, LMGetKbdType(),
                                   kUCKeyTranslateNoDeadKeysMask, &state, 4, &count, value) == noErr &&
                    count == 1 && QChar(value[0]).toUpper() == QLatin1Char(char(ascii)))
                    found = hidKey(DesktopOS::macOS, code);
            }
        }
        CFRelease(input);
        return found;
    }

    bool text(const QString &value) override
    {
        for (qsizetype i = 0; i < value.size();) {
            const int n =
                value[i].isHighSurrogate() && i + 1 < value.size() && value[i + 1].isLowSurrogate() ? 2 : 1;
            for (bool down : {true, false}) {
                auto event = CGEventCreateKeyboardEvent(source_, 0, down);
                if (!event)
                    return false;
                CGEventKeyboardSetUnicodeString(event, n,
                                                reinterpret_cast<const UniChar *>(value.utf16() + i));
                if (!post(event, 0))
                    return false;
            }
            i += n;
        }
        return true;
    }

    bool pointer(ScreenPoint p, MouseButton button, InputKind kind) override
    {
        int index = int(button) - 1;
        const bool down = kind == InputKind::ButtonDown, move = kind == InputKind::Move;
        if (move && buttons_)
            for (index = 0; !(buttons_ & (1u << index)); ++index) {
            }
        constexpr CGEventType pressed[] = {kCGEventLeftMouseDown, kCGEventRightMouseDown,
                                           kCGEventOtherMouseDown};
        constexpr CGEventType lifted[] = {kCGEventLeftMouseUp, kCGEventRightMouseUp, kCGEventOtherMouseUp};
        constexpr CGEventType dragged[] = {kCGEventLeftMouseDragged, kCGEventRightMouseDragged,
                                           kCGEventOtherMouseDragged};
        const int t = std::min(index, 2);
        auto type = move ? (buttons_ ? dragged[t] : kCGEventMouseMoved) : (down ? pressed[t] : lifted[t]);
        auto point = location(p, display_);
        auto event = CGEventCreateMouseEvent(source_, type, point, CGMouseButton(index));
        if (!event)
            return false;
        if (!move) {
            auto &click = clicks_[index];
            if (down) {
                auto now = NSProcessInfo.processInfo.systemUptime;
                bool successive = now - click.time <= NSEvent.doubleClickInterval &&
                                  std::hypot(point.x - click.point.x, point.y - click.point.y) <= 4.;
                click = {point, now, successive ? click.count + 1 : 1};
                buttons_ |= quint8(1u << index);
            } else
                buttons_ &= ~quint8(1u << index);
            CGEventSetIntegerValueField(event, kCGMouseEventClickState, click.count);
        }
        return post(event, flagsFor(mods_));
    }

    bool wheel(ScreenPoint p, qint16 x, qint16 y, bool pixels) override
    {
        auto units = [pixels](int n) { return pixels ? n : qRound(n * 0.30); };
        auto event = CGEventCreateScrollWheelEvent(source_, kCGScrollEventUnitPixel, 2, units(y), units(x));
        if (!event)
            return false;
        CGEventSetLocation(event, location(p, display_));
        return post(event, flagsFor(mods_));
    }
};

CFTypeRef attribute(AXUIElementRef element, CFStringRef name)
{
    CFTypeRef result = nullptr;
    if (element)
        AXUIElementCopyAttributeValue(element, name, &result);
    return result;
}
} // namespace

quint64 clipboardRevision()
{
    return quint64(NSPasteboard.generalPasteboard.changeCount);
}

void watchDesktopPower(QObject *context, std::function<void(bool)> callback)
{
    auto center = NSWorkspace.sharedWorkspace.notificationCenter;
    auto add = [center, callback](NSNotificationName name, bool awake) {
        return [center addObserverForName:name
                                   object:nil
                                    queue:NSOperationQueue.mainQueue
                               usingBlock:^(NSNotification *) {
                                 callback(awake);
                               }];
    };
    auto sleep = add(NSWorkspaceWillSleepNotification, false);
    auto wake = add(NSWorkspaceDidWakeNotification, true);
    QObject::connect(context, &QObject::destroyed, [center, sleep, wake] {
        [center removeObserver:sleep];
        [center removeObserver:wake];
    });
}

DesktopPermissions checkDesktopPermissions()
{
    return {CGPreflightScreenCaptureAccess(), AXIsProcessTrusted() != 0};
}

bool openSystemPermissionSettings(PermissionKind kind)
{
    @autoreleasepool {
        NSString *address = nil;
        switch (kind) {
        case PermissionKind::ScreenRecording:
            address = @"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture";
            break;
        case PermissionKind::Accessibility:
            address = @"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility";
            break;
        case PermissionKind::LocalNetwork:
            address = @"x-apple.systempreferences:com.apple.preference.security?Privacy_LocalNetwork";
            break;
        }
        return address && [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:address]];
    }
}

bool allowRemoteInput(QString &reason)
{
    NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt : @YES};
    if (AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options))
        return true;
    reason = QStringLiteral("请在「系统设置 → 隐私与安全性 → 辅助功能」允许 WWMM Remote，再请求控制。");
    return false;
}

std::unique_ptr<InputSink> createInputSink(quint64 display)
{
    return std::make_unique<MacInput>(display ? CGDirectDisplayID(display) : CGMainDisplayID());
}

InputFocus focusedInputKind()
{
    @autoreleasepool {
        auto app = NSWorkspace.sharedWorkspace.frontmostApplication;
        if (!app)
            return InputFocus::Raw;
        QString id = QString::fromNSString(app.bundleIdentifier).toLower();
        if (id == "com.apple.terminal" || id == "com.googlecode.iterm2" || id.contains("ghostty") ||
            id.contains("alacritty") || id.contains("wezterm"))
            return InputFocus::Terminal;
        auto ax = AXUIElementCreateApplication(app.processIdentifier);
        if (!ax)
            return InputFocus::Raw;
        AXUIElementSetMessagingTimeout(ax, 0.08);
        auto element = static_cast<AXUIElementRef>(attribute(ax, kAXFocusedUIElementAttribute));
        CFRelease(ax);
        QString role, subrole;
        if (element && CFGetTypeID(element) == AXUIElementGetTypeID()) {
            AXUIElementSetMessagingTimeout(element, 0.08);
            auto r = attribute(element, kAXRoleAttribute), sub = attribute(element, kAXSubroleAttribute);
            if (r && CFGetTypeID(r) == CFStringGetTypeID())
                role = QString::fromCFString(static_cast<CFStringRef>(r));
            if (sub && CFGetTypeID(sub) == CFStringGetTypeID())
                subrole = QString::fromCFString(static_cast<CFStringRef>(sub));
            if (r)
                CFRelease(r);
            if (sub)
                CFRelease(sub);
        }
        if (element)
            CFRelease(element);
        if (subrole == "AXSecureTextField")
            return InputFocus::Raw;
        if (role == "AXTextField" || role == "AXTextArea" || role == "AXComboBox" || role == "AXSearchField")
            return InputFocus::Text;
        if (id == "com.apple.textedit" || id == "com.apple.notes" || id == "com.microsoft.word" ||
            id == "com.microsoft.excel" || id == "com.apple.iwork.pages" || id == "com.apple.iwork.numbers")
            return InputFocus::Text;
        if (id == "com.apple.finder" || id == "com.apple.safari" || id.contains("chrome") ||
            id.contains("firefox") || id.contains("microsoft.") || id.contains("iwork."))
            return InputFocus::Office;
        return InputFocus::Raw;
    }
}
} // namespace wwmm
