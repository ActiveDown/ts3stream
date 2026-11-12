#pragma once

#include <QHash>
#include <QModelIndex>
#include <QPoint>
#include <QRect>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QStyledItemDelegate>

class QPainter;
class QTreeView;

namespace ts3stream::ui {

struct LiveStreamInfo {
    QString room;
    QString identity;     // TS3 unique id / LiveKit identity
    QString nickname;
    int channelId = 0;
};

class LiveBadgeDelegate : public QStyledItemDelegate {
public:
    explicit LiveBadgeDelegate(QAbstractItemDelegate* inner, QObject* parent = nullptr);

    void setStreams(const QHash<QString, LiveStreamInfo>& streamsByNickname);
    QHash<QString, LiveStreamInfo> streams() const;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    QPointer<QAbstractItemDelegate> inner_;
    QHash<QString, LiveStreamInfo> streams_;
};

// Returns true and fills *out when the item under viewportPos is a live streamer.
QRect liveBadgeRect(const QRect& rowRect, const QRect& viewportRect);
bool liveStreamAt(QTreeView* view, const QPoint& viewportPos, LiveStreamInfo* out);

} // namespace ts3stream::ui
