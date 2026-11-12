#include "active_streams_button.h"
#include <QAction>
#include <QMenu>
#include <algorithm>

namespace ts3stream::ui {

ActiveStreamsButton::ActiveStreamsButton(QWidget* parent) : QToolButton(parent) {
    setObjectName(QStringLiteral("ts3stream_active_streams"));
    setAutoRaise(true);
    setPopupMode(QToolButton::InstantPopup);
    setToolTip(QStringLiteral("Трансляции на текущем сервере TeamSpeak"));
    auto* popup = new QMenu(this);
    setMenu(popup);
    QObject::connect(popup, &QMenu::aboutToShow, this, [this, popup] {
        popup->clear();
        for (const LiveStreamInfo& stream : streams_) {
            auto* action = popup->addAction(QStringLiteral("%1 — канал %2").arg(stream.nickname).arg(stream.channelId));
            QObject::connect(action, &QAction::triggered, this, [this, stream] {
                if (onWatch_) onWatch_(stream);
            });
        }
    });
    setStreams({});
}

void ActiveStreamsButton::setStreams(QVector<LiveStreamInfo> streams) {
    std::sort(streams.begin(), streams.end(), [](const LiveStreamInfo& a, const LiveStreamInfo& b) {
        if (a.channelId != b.channelId) return a.channelId < b.channelId;
        return a.nickname.localeAwareCompare(b.nickname) < 0;
    });
    streams_ = std::move(streams);
    setText(QStringLiteral("Идущие стримы (%1)").arg(streams_.size()));
    setVisible(!streams_.isEmpty());
    if (streams_.isEmpty()) menu()->close();
}

void ActiveStreamsButton::setOnWatch(std::function<void(LiveStreamInfo)> callback) {
    onWatch_ = std::move(callback);
}

} // namespace ts3stream::ui
