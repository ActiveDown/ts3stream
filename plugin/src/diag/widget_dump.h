#pragma once

#include <QString>

namespace ts3stream::diag {

// Walks the Qt widget tree of the running TeamSpeak client and writes a report.
void dumpWidgetTree(const QString& filePath);

} // namespace ts3stream::diag
