#pragma once

#include <QThread>
#include <array>
#include <atomic>
#include <qt_windows.h>

namespace wwmm {

// Lives only while an authorized Mac control surface has keyboard focus.
class WindowsKeyboardGrab final : public QThread {
    Q_OBJECT
  public:
    explicit WindowsKeyboardGrab(HWND surface, bool acceptsText);
    ~WindowsKeyboardGrab() override;
    void setAcceptsText(bool value) { acceptsText_.store(value); }
    bool installed() const { return installed_.load(); }

  signals:
    void key(quint8 usage, bool down, quint8 modifiers);
    void releaseRequested();
    void unavailable();

  private:
    void run() override;
    bool hasFocus() const;
    bool handle(const KBDLLHOOKSTRUCT &event, bool down);
    static LRESULT CALLBACK keyboardProc(int code, WPARAM message, LPARAM data);
    static thread_local WindowsKeyboardGrab *current_;
    HWND surface_;
    std::atomic_bool acceptsText_, installed_{false};
    enum class Route : quint8 { Up, Local, Remote };
    std::array<Route, 256> routes_{};
    quint8 modifiers_ = 0, preexisting_ = 0;
    bool releasing_ = false;
};
} // namespace wwmm
