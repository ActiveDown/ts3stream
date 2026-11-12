#include "live_stream_menu.h"

#include <QAction>
#include <QMenu>

namespace ts3stream::ui {

QMenu* popupLiveStreamMenu(QWidget* parent, LiveStreamInfo stream, const QPoint& position,
                          std::function<void(LiveStreamInfo)> onWatch) {
    auto* menu = new QMenu(parent);
    QAction* watch = menu->addAction(QStringLiteral("▶  Смотреть стрим — %1").arg(stream.nickname));
    QObject::connect(watch, &QAction::triggered, menu,
        [stream = std::move(stream), onWatch = std::move(onWatch)] { onWatch(stream); });
    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(position);
    return menu;
}

} // namespace ts3stream::ui
