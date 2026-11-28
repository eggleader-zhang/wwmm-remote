#pragma once
#include "desktop_input.h"
#include <QObject>
#include <functional>

namespace wwmm {
struct DesktopPermissions {
    bool screenRecording = true;
    bool accessibility = true;
};
enum class PermissionKind { ScreenRecording, Accessibility, LocalNetwork };
DesktopPermissions checkDesktopPermissions();
bool openSystemPermissionSettings(PermissionKind kind);

bool allowRemoteInput(QString &reason);
std::unique_ptr<InputSink> createInputSink(quint64 display = 0);
InputFocus focusedInputKind();
quint64 clipboardRevision();
void watchDesktopPower(QObject *context, std::function<void(bool awake)> callback);
} // namespace wwmm
