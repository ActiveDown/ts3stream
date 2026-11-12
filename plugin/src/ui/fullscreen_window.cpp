#include "fullscreen_window.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QWindow>

#include <Windows.h>

namespace ts3stream::ui {

void showFullscreenOnCurrentMonitor(QWidget* panel) {
    QWidget* host = panel->window();
    // Read the host's native monitor before detaching the viewer. Qt screen
    // caches and logical coordinates alone are insufficient after reparenting.
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    const bool haveMonitor = GetMonitorInfoW(
        MonitorFromWindow(reinterpret_cast<HWND>(host->winId()), MONITOR_DEFAULTTONEAREST),
        &monitor) != FALSE;
    QScreen* target = QGuiApplication::screenAt(host->frameGeometry().center());
    if (haveMonitor) {
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen->name().compare(QString::fromWCharArray(monitor.szDevice),
                                       Qt::CaseInsensitive) == 0) {
                target = screen;
                break;
            }
        }
    }
    if (!target) target = host->screen();
    if (!target) target = QGuiApplication::primaryScreen();
    panel->setParent(nullptr);
    panel->setWindowFlags(Qt::Window);
    panel->setWindowState(Qt::WindowNoState);
    panel->winId();
    if (target) {
        panel->windowHandle()->setScreen(target);
        panel->setGeometry(target->geometry());
    }
    panel->showFullScreen();
    if (haveMonitor) {
        // Qt 5 can move a newly detached fullscreen widget to the primary
        // monitor even after setScreen/setGeometry. Apply the native rectangle
        // AFTER that transition. Both Win32 calls use the same DPI context;
        // never pass these coordinates through Qt's logical-pixel conversion.
        const RECT& rect = monitor.rcMonitor;
        SetWindowPos(reinterpret_cast<HWND>(panel->winId()), nullptr,
                     rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    panel->raise();
}

void restoreEmbeddedWindow(QWidget* panel, QWidget* parent) {
    panel->setWindowState(Qt::WindowNoState);
    panel->setParent(parent);
    panel->setWindowFlags(Qt::Widget);
    panel->show();
}

void showDetachedWindow(QWidget* panel, const QByteArray& savedGeometry) {
    const QPoint origin = panel->mapToGlobal(QPoint(0, 0));
    const QSize size = panel->size().boundedTo(QSize(1200, 800));
    QScreen* screen = panel->screen();
    panel->setWindowState(Qt::WindowNoState);
    panel->setParent(nullptr);
    panel->setWindowFlags(Qt::Window);
    panel->setAttribute(Qt::WA_QuitOnClose, false);
    panel->winId();
    if (screen) panel->windowHandle()->setScreen(screen);
    if (savedGeometry.isEmpty() || !panel->restoreGeometry(savedGeometry)) {
        QRect geometry(origin, size);
        if (screen) {
            const QRect available = screen->availableGeometry().adjusted(12, 32, -12, -12);
            geometry.setSize(geometry.size().boundedTo(available.size()));
            geometry.moveLeft(qBound(available.left(), geometry.left(),
                                     available.right() - geometry.width() + 1));
            geometry.moveTop(qBound(available.top(), geometry.top(),
                                    available.bottom() - geometry.height() + 1));
        }
        panel->setGeometry(geometry);
    }
    panel->show();
    panel->raise();
}

} // namespace ts3stream::ui
