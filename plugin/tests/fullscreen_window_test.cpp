#include "ui/fullscreen_window.h"
#include "ui/stream_panel.h"

#include <QApplication>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <QWindow>

#include <Windows.h>
#include <cstdio>

namespace {
MONITORINFOEXW monitorInfo(QWidget* widget) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromWindow(reinterpret_cast<HWND>(widget->winId()),
                                     MONITOR_DEFAULTTONEAREST), &info);
    return info;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QWidget host;
    host.setWindowTitle(QStringLiteral("TS3Stream fullscreen regression test"));
    ts3stream::ui::StreamPanel panel(&host);
    panel.setMinimumSize(320, 180);
    int step = 0;
    int failures = 0;
    MONITORINFOEXW expected{};
    QByteArray savedGeometry;
    QRect savedRect;
    QRect hostRect;
    bool wasMaximized = false;
    bool detachClicked = false;
    bool closeClicked = false;
    panel.setOnDetach([&] { detachClicked = true; });
    panel.setOnClose([&] { closeClicked = true; });
    auto* detach = panel.findChild<QToolButton*>(QStringLiteral("ts3stream_detach"));
    if (!detach) return 2;
    detach->click();
    if (!detachClicked) return 3;
    QTimer timer;
    // Exercise real native windows, including delayed Qt geometry events.
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        const auto screens = QGuiApplication::screens();
        const int cycle = step / 6;
        if (cycle >= screens.size() * 3) {
            panel.close();
            if (!closeClicked) ++failures;
            std::printf("fullscreen cycles=%d failures=%d\n", cycle, failures);
            app.exit(failures ? 1 : 0);
            return;
        }
        QScreen* screen = screens[cycle % screens.size()];
        const bool detached = cycle >= screens.size();
        switch (step % 6) {
        case 0: {
            host.showNormal();
            host.winId();
            host.windowHandle()->setScreen(screen);
            host.setGeometry(QRect(screen->availableGeometry().topLeft() + QPoint(60, 60),
                                   QSize(640, 480)));
            host.show();
            panel.setGeometry(host.rect());
            panel.show();
            break;
        }
        case 1: {
            if (detached) {
                ts3stream::ui::showDetachedWindow(&panel);
                panel.setDetachedMode(true);
                QScreen* other = screens[(cycle + 1) % screens.size()];
                panel.windowHandle()->setScreen(other);
                panel.setGeometry(QRect(other->availableGeometry().topLeft() + QPoint(80, 80),
                                        QSize(800, 600)));
                if (cycle >= screens.size() * 2) panel.showMaximized();
                if (panel.parentWidget() || !panel.isWindow() ||
                    detach->text() != QStringLiteral("Встроить")) ++failures;
            }
            break;
        }
        case 2:
            expected = monitorInfo(panel.window());
            savedGeometry = panel.saveGeometry();
            savedRect = panel.geometry();
            wasMaximized = panel.isMaximized();
            hostRect = host.frameGeometry();
            panel.setFullscreenMode(true);
            ts3stream::ui::showFullscreenOnCurrentMonitor(&panel);
            break;
        case 3: {
            const auto actual = monitorInfo(&panel);
            RECT rect{};
            GetWindowRect(reinterpret_cast<HWND>(panel.winId()), &rect);
            const bool ok = QString::fromWCharArray(expected.szDevice) ==
                                QString::fromWCharArray(actual.szDevice) &&
                            EqualRect(&rect, &expected.rcMonitor);
            std::printf("cycle=%d expected=%ls actual=%ls rect=%ld,%ld %ldx%ld %s\n",
                        cycle, expected.szDevice, actual.szDevice, rect.left, rect.top,
                        rect.right - rect.left, rect.bottom - rect.top, ok ? "PASS" : "FAIL");
            if (!ok) ++failures;
            panel.setFullscreenMode(false);
            if (detached) {
                ts3stream::ui::showDetachedWindow(&panel, savedGeometry);
            } else {
                ts3stream::ui::restoreEmbeddedWindow(&panel, &host);
                panel.setGeometry(host.rect());
            }
            break;
        }
        case 4:
            if (detached) {
                const auto actual = monitorInfo(&panel);
                if (!panel.isWindow() || panel.isFullScreen() || panel.isMaximized() != wasMaximized ||
                    panel.geometry() != savedRect || QString::fromWCharArray(actual.szDevice) !=
                        QString::fromWCharArray(expected.szDevice)) {
                    std::printf("detached restore FAIL cycle=%d\n", cycle);
                    ++failures;
                }
                ts3stream::ui::restoreEmbeddedWindow(&panel, &host);
                panel.setDetachedMode(false);
                panel.setGeometry(host.rect());
            }
            if (host.frameGeometry() != hostRect || !host.isVisible() || !host.isEnabled()) {
                std::printf("TS3 host window changed FAIL cycle=%d\n", cycle);
                ++failures;
            }
            break;
        case 5:
            if (panel.isWindow() || panel.isFullScreen() || panel.parentWidget() != &host) {
                std::printf("embedded restore FAIL: window=%d fullscreen=%d\n",
                            panel.isWindow(), panel.isFullScreen());
                ++failures;
            }
            break;
        }
        ++step;
    });
    timer.start(150);
    return app.exec();
}
