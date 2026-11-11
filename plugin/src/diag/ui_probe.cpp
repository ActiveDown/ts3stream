#include "ui_probe.h"

#include "../log.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QLayout>
#include <QMainWindow>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

#include <algorithm>

namespace ts3stream::diag {
namespace {

QPointer<QToolButton> g_probeButton;

QIcon makeProbeIcon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(QColor(220, 40, 40));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(2, 2, 20, 20);
    painter.setPen(QPen(Qt::white, 3));
    painter.drawLine(7, 12, 17, 12);
    painter.drawLine(12, 7, 12, 17);
    painter.end();
    return QIcon(pixmap);
}

QWidget* findMainWindow() {
    QWidget* best = nullptr;
    qint64 bestArea = 0;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (!widget || !widget->isWindow() || !widget->isVisible()) {
            continue;
        }
        const qint64 area = static_cast<qint64>(widget->width()) * widget->height();
        const QString title = widget->windowTitle();
        const QString className = QString::fromLatin1(widget->metaObject()->className());
        log("top-level: title=\"%s\" class=%s size=%dx%d area=%lld",
            title.toUtf8().constData(), className.toUtf8().constData(),
            widget->width(), widget->height(), static_cast<long long>(area));
        if (title.contains("TeamSpeak", Qt::CaseInsensitive)) {
            return widget;
        }
        if (area > bestArea) {
            bestArea = area;
            best = widget;
        }
    }
    return best;
}

QToolButton* createProbeButton(QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName("ts3stream_probe_button");
    button->setText("TS3Stream");
    button->setToolTip("TS3Stream diagnostics probe");
    button->setIcon(makeProbeIcon());
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

bool addToToolbar(QWidget* mainWindow) {
    auto* toolBar = mainWindow->findChild<QToolBar*>();
    if (!toolBar) {
        const auto toolBars = mainWindow->findChildren<QToolBar*>();
        if (!toolBars.isEmpty()) {
            toolBar = toolBars.first();
        }
    }
    if (!toolBar) {
        return false;
    }

    log("found QToolBar object=\"%s\" class=%s size=%dx%d",
        toolBar->objectName().toUtf8().constData(),
        toolBar->metaObject()->className(),
        toolBar->width(), toolBar->height());

    QToolButton* button = createProbeButton(toolBar);
    QAction* action = toolBar->addWidget(button);
    if (!action) {
        log("toolbar->addWidget returned null");
        button->deleteLater();
        return false;
    }
    g_probeButton = button;
    log("probe button added to QToolBar");
    return true;
}

bool addToTopContainer(QWidget* mainWindow) {
    const auto buttons = mainWindow->findChildren<QToolButton*>();
    if (buttons.isEmpty()) {
        return false;
    }

    QToolButton* topButton = nullptr;
    int bestY = 1000000;
    for (QToolButton* button : buttons) {
        if (!button || !button->isVisible()) {
            continue;
        }
        const QPoint pos = button->mapTo(mainWindow, QPoint(0, 0));
        if (pos.y() < bestY) {
            bestY = pos.y();
            topButton = button;
        }
    }
    if (!topButton || !topButton->parentWidget()) {
        return false;
    }

    QWidget* container = topButton->parentWidget();
    if (auto* layout = container->layout()) {
        QToolButton* probe = createProbeButton(container);
        layout->addWidget(probe);
        g_probeButton = probe;
        log("probe button added to top container class=%s layout=%s",
            container->metaObject()->className(),
            layout->metaObject()->className());
        return true;
    }

    return false;
}

bool addAsOverlay(QWidget* mainWindow) {
    QToolButton* probe = createProbeButton(mainWindow);
    probe->setGeometry(std::max(8, mainWindow->width() - 140), 8, 130, 28);
    probe->raise();
    probe->show();
    g_probeButton = probe;
    log("probe button added as overlay child of main window");
    return true;
}

} // namespace

void probeUi() {
    QWidget* mainWindow = findMainWindow();
    if (!mainWindow) {
        log("probeUi: main window not found");
        return;
    }

    log("probeUi: main window title=\"%s\" class=%s size=%dx%d",
        mainWindow->windowTitle().toUtf8().constData(),
        mainWindow->metaObject()->className(),
        mainWindow->width(), mainWindow->height());

    if (auto* mainWindowAsMain = qobject_cast<QMainWindow*>(mainWindow)) {
        log("main window is QMainWindow");
        const auto toolBars = mainWindowAsMain->findChildren<QToolBar*>();
        log("QMainWindow toolbars=%lld menus=%lld",
            static_cast<long long>(toolBars.size()),
            static_cast<long long>(mainWindowAsMain->findChildren<QMenuBar*>().size()));
    }

    if (addToToolbar(mainWindow)) {
        return;
    }
    log("QToolBar not found, trying top container");
    if (addToTopContainer(mainWindow)) {
        return;
    }
    log("top container not found, using overlay");
    addAsOverlay(mainWindow);
}

void removeProbeUi() {
    if (g_probeButton) {
        log("removing probe button");
        delete g_probeButton.data();
        g_probeButton = nullptr;
    }
}

} // namespace ts3stream::diag
