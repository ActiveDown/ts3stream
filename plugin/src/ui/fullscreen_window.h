#pragma once

#include <QByteArray>

class QWidget;

namespace ts3stream::ui {

// Detach an embedded viewer and keep fullscreen on its current host monitor.
void showFullscreenOnCurrentMonitor(QWidget* panel);
void restoreEmbeddedWindow(QWidget* panel, QWidget* parent);
void showDetachedWindow(QWidget* panel, const QByteArray& savedGeometry = {});

} // namespace ts3stream::ui
