#pragma once

#include <QImage>
#include <QSize>

namespace ts3stream::ui {

inline QSize videoResolutionBounds(int height) {
    switch (height) {
    case 720: return QSize(1280, 720);
    case 1440: return QSize(2560, 1440);
    default: return QSize(1920, 1080);
    }
}

inline QImage scaleStreamFrame(const QImage& image, const QSize& bounds) {
    if (image.width() <= bounds.width() && image.height() <= bounds.height()) return image;
    return image.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace ts3stream::ui
