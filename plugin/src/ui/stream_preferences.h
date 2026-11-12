#pragma once

#include "source_picker.h"

namespace ts3stream::ui {

// Only video quality is persisted; source handles and audio selection are not.
void loadStreamPreferences(CaptureSource& source);
bool saveStreamPreferences(const CaptureSource& source);

} // namespace ts3stream::ui
