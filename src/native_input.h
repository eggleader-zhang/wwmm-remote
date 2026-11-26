#pragma once
#include "desktop_input.h"
#include <QObject>
#include <functional>

namespace wwmm {
bool allowRemoteInput(QString &reason);
std::unique_ptr<InputSink> createInputSink(quint64 display = 0);
InputFocus focusedInputKind();
quint64 clipboardRevision();
void watchDesktopPower(QObject *context, std::function<void(bool awake)> callback);
} // namespace wwmm
