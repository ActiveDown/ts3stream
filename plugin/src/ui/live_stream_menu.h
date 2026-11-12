#pragma once

#include "live_badge_delegate.h"
#include <functional>

class QMenu;

namespace ts3stream::ui {

// Own the selection data: registry refreshes may invalidate all hash iterators
// and references while a popup is open. No nested event loop is needed.
QMenu* popupLiveStreamMenu(QWidget* parent, LiveStreamInfo stream, const QPoint& position,
                          std::function<void(LiveStreamInfo)> onWatch);

} // namespace ts3stream::ui
