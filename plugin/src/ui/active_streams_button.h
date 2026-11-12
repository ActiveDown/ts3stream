#pragma once

#include "live_badge_delegate.h"
#include <QToolButton>
#include <QVector>
#include <functional>

namespace ts3stream::ui {

class ActiveStreamsButton : public QToolButton {
public:
    explicit ActiveStreamsButton(QWidget* parent = nullptr);
    void setStreams(QVector<LiveStreamInfo> streams);
    void setOnWatch(std::function<void(LiveStreamInfo)> callback);
private:
    QVector<LiveStreamInfo> streams_;
    std::function<void(LiveStreamInfo)> onWatch_;
};

} // namespace ts3stream::ui
