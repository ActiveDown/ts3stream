#include "ui/stream_panel.h"
#include <QApplication>
#include <QCursor>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <cstdio>

static void waitForEvents(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char** argv) {
    // Isolate cursor-based hover assertions from real desktop mouse input.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QPoint originalCursor = QCursor::pos();
    QWidget host;
    host.resize(900, 650);
    ts3stream::ui::StreamPanel panel(&host);
    panel.setGeometry(host.rect());
    host.show();
    auto* audio = panel.findChild<QWidget*>(QStringLiteral("ts3stream_playback_audio"));
    auto* members = panel.findChild<QWidget*>(QStringLiteral("ts3stream_members"));
    auto* slider = panel.findChild<QSlider*>(QStringLiteral("ts3stream_volume"));
    auto* idle = panel.findChild<QTimer*>(QStringLiteral("ts3stream_controls_idle"));
    QToolButton* hide = nullptr;
    QToolButton* show = nullptr;
    for (auto* button : panel.findChildren<QToolButton*>()) {
        if (button->text() == QStringLiteral("Скрыть")) hide = button;
        if (button->text() == QStringLiteral("▲ Участники")) show = button;
    }
    if (!audio || !members || !slider || !idle || !hide || !show) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    check(idle->interval() == 3000, "three-second idle delay");
    idle->setInterval(70);
    auto move = [&](QWidget* target, QPoint point) {
        const QPoint global = target->mapToGlobal(point);
        QCursor::setPos(global);
        QMouseEvent event(QEvent::MouseMove, point, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    };
    for (bool detached : {false, true}) {
        if (detached) {
            panel.setParent(nullptr);
            panel.setWindowFlag(Qt::Window, true);
            panel.resize(900, 650);
            panel.show();
        }
        move(&panel, QPoint(300, 200));
        panel.setFullscreenMode(true);
        check(audio->isVisible() && members->isVisible(), "controls visible on entering fullscreen");
        waitForEvents(180);
        check(audio->isHidden() && members->isHidden() && show->isHidden(), "all overlays hidden when idle");
        panel.resize(910, 660);
        panel.setMembers({{QStringLiteral("Alice"), true, false}});
        check(audio->isHidden() && members->isHidden(), "geometry/member updates do not wake controls");
        move(&panel, QPoint(310, 210));
        check(audio->isVisible() && members->isVisible(), "mouse movement restores overlays");
        move(slider, slider->rect().center());
        waitForEvents(180);
        check(audio->isVisible(), "hovering slider prevents hiding");
        slider->setSliderDown(true);
        move(&panel, QPoint(320, 220));
        waitForEvents(180);
        check(audio->isVisible(), "dragging slider outside controls prevents hiding");
        slider->setSliderDown(false);
        waitForEvents(180);
        check(audio->isHidden(), "overlays hide after drag ends");
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(&panel, &key);
        check(audio->isVisible(), "keyboard restores controls");
        hide->click();
        check(members->isHidden() && show->isVisible(), "manual members toggle");
        waitForEvents(180);
        check(audio->isHidden() && show->isHidden(), "collapsed members button also auto-hides");
        move(&panel, QPoint(330, 230));
        check(audio->isVisible() && show->isVisible() && members->isHidden(), "wake preserves manual members choice");
        waitForEvents(180);
        panel.setFullscreenMode(false);
        check(audio->isVisible() && !idle->isActive(), "exit restores embedded/detached controls and stops timer");
        waitForEvents(180);
        check(audio->isVisible(), "normal mode controls stay visible");
        panel.setFullscreenMode(true);
        show->click();
        check(members->isVisible(), "members can be restored");
        panel.setFullscreenMode(false);
    }
    QCursor::setPos(originalCursor);
    std::printf("fullscreen auto-hide: failures=%d\n", failures);
    return failures ? 1 : 0;
}
