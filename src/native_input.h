#pragma once
#include "desktop_input.h"

namespace wwmm {
bool allowRemoteInput(QString &reason);
std::unique_ptr<InputSink> createInputSink();
InputFocus focusedInputKind();
} // namespace wwmm
