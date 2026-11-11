#include "live_badge_delegate.h"

#include <QAbstractItemDelegate>
#include <QColor>
#include <QFont>
#include <QPainter>
#include <QRect>
#include <QStyleOptionViewItem>
#include <QTreeView>

namespace ts3stream::ui {
namespace {

constexpr int kBadgeWidth = 38;
constexpr int kBadgeHeight = 14;
constexpr int kBadgeRightMargin = 4;
constexpr int kBadgeRadius = 3;
constexpr int kBadgeFontPixelSize = 9;

const QColor kBadgeColor(0xd9, 0x30, 0x25);

QString liveNickname(const QModelIndex& index) {
    return index.data(Qt::DisplayRole).toString().trimmed();
}

} // namespace

LiveBadgeDelegate::LiveBadgeDelegate(QAbstractItemDelegate* inner, QObject* parent)
    : QStyledItemDelegate(parent), inner_(inner) {}

void LiveBadgeDelegate::setStreams(const QHash<QString, LiveStreamInfo>& streamsByNickname) {
    streams_ = streamsByNickname;
}

QHash<QString, LiveStreamInfo> LiveBadgeDelegate::streams() const {
    return streams_;
}

void LiveBadgeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (inner_) {
        inner_->paint(painter, option, index);
    } else {
        QStyledItemDelegate::paint(painter, option, index);
    }

    if (!painter || !index.isValid() || streams_.isEmpty()) {
        return;
    }

    const QString nickname = liveNickname(index);
    if (nickname.isEmpty() || !streams_.contains(nickname)) {
        return;
    }

    if (option.rect.width() < kBadgeWidth + 2 * kBadgeRightMargin ||
        option.rect.height() < kBadgeHeight) {
        return;
    }

    const QRect badgeRect(option.rect.right() - kBadgeRightMargin - kBadgeWidth,
                          option.rect.center().y() - kBadgeHeight / 2,
                          kBadgeWidth,
                          kBadgeHeight);

    painter->save();
    painter->setClipRect(option.rect, Qt::IntersectClip);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(kBadgeColor);
    painter->drawRoundedRect(badgeRect, kBadgeRadius, kBadgeRadius);

    QFont font = painter->font();
    font.setBold(true);
    font.setPixelSize(kBadgeFontPixelSize);
    painter->setFont(font);
    painter->setPen(Qt::white);
    painter->drawText(badgeRect, Qt::AlignCenter, QStringLiteral("LIVE"));
    painter->restore();
}

QSize LiveBadgeDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (inner_) {
        return inner_->sizeHint(option, index);
    }
    return QStyledItemDelegate::sizeHint(option, index);
}

bool liveStreamAt(QTreeView* view, const QPoint& viewportPos, LiveStreamInfo* out) {
    QTreeView* treeView = qobject_cast<QTreeView*>(view);
    if (!treeView) {
        return false;
    }

    const QModelIndex index = treeView->indexAt(viewportPos);
    if (!index.isValid()) {
        return false;
    }

    // No Q_OBJECT in this header (the plugin builds without AUTOMOC), so use RTTI
    // rather than qobject_cast for the delegate type check.
    auto* delegate = dynamic_cast<LiveBadgeDelegate*>(treeView->itemDelegate(index));
    if (!delegate) {
        return false;
    }

    const QString nickname = liveNickname(index);
    if (nickname.isEmpty()) {
        return false;
    }

    const QHash<QString, LiveStreamInfo> streams = delegate->streams();
    const auto it = streams.constFind(nickname);
    if (it == streams.constEnd()) {
        return false;
    }

    if (out) {
        *out = it.value();
    }
    return true;
}

} // namespace ts3stream::ui
