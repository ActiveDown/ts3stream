#pragma once

namespace ts3stream::diag {

// Tries to locate the main TS3 window and add a visible probe button.
// Runs on the Qt GUI thread.
void probeUi();

// Removes the probe button if it still exists. Runs on the Qt GUI thread.
void removeProbeUi();

} // namespace ts3stream::diag
