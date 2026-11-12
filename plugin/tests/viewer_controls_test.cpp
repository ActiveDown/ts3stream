#include "media/playback_gain.h"
#include "ui/stream_panel.h"
#include "ui/active_streams_button.h"
#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QSlider>
#include <QTimer>
#include <cstdio>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    const int16_t samples[] = {-32768, -1234, -1, 0, 1, 1234, 32767};
    const int16_t half[] = {-16384, -617, 0, 0, 0, 617, 16383};
    for (int i = 0; i < 7; ++i) {
        check(ts3stream::media::applyPlaybackGain(samples[i], 0) == 0, "mute must output silence");
        check(ts3stream::media::applyPlaybackGain(samples[i], 50) == half[i], "50% PCM gain");
        check(ts3stream::media::applyPlaybackGain(samples[i], 100) == samples[i], "100% PCM unchanged");
        check(ts3stream::media::applyPlaybackGain(samples[i], -10) == 0, "lower clamp");
        check(ts3stream::media::applyPlaybackGain(samples[i], 200) == samples[i], "upper clamp");
    }
    QWidget host;
    host.resize(900, 650);
    ts3stream::ui::StreamPanel panel(&host);
    panel.setGeometry(host.rect());
    host.show();
    panel.show();
    auto* mute = panel.findChild<QToolButton*>(QStringLiteral("ts3stream_mute"));
    auto* slider = panel.findChild<QSlider*>(QStringLiteral("ts3stream_volume"));
    auto* controls = panel.findChild<QWidget*>(QStringLiteral("ts3stream_playback_audio"));
    if (!mute || !slider || !controls) return 2;
    int volume = -1, notifications = 0;
    bool muted = false;
    panel.setOnPlaybackAudioChanged([&](int v, bool m) { volume = v; muted = m; ++notifications; });
    slider->setValue(37);
    check(volume == 37 && !muted && notifications == 1, "slider callback");
    mute->click();
    check(volume == 37 && muted && notifications == 2, "mute preserves volume");
    panel.setFullscreenMode(true);
    check(controls->parentWidget() == &panel && controls->isVisible(), "fullscreen audio controls visible");
    for (auto* button : panel.findChildren<QToolButton*>()) {
        if (button->text() == QStringLiteral("Скрыть")) button->click();
    }
    check(controls->isVisible() && panel.rect().contains(controls->geometry()), "hidden members keep audio accessible");
    mute->click();
    check(volume == 37 && !muted && notifications == 3, "unmute restores volume in fullscreen");
    panel.setFullscreenMode(false);
    check(controls->isVisible() && controls->parentWidget() != &panel && slider->value() == 37,
          "embedded controls restored without resetting level");

    for (int width : {80, 120, 180, 600}) {
        const QRect viewport(0, 0, width, 300);
        for (int left : {-100, 0, 30}) {
            const QRect badge = ts3stream::ui::liveBadgeRect(QRect(left, 40, 800, 20), viewport);
            check(badge.width() == 38 && viewport.contains(badge), "LIVE follows visible viewport, not column width");
        }
    }

    ts3stream::ui::ActiveStreamsButton active(&host);
    check(active.isHidden(), "empty stream list hides toolbar button");
    ts3stream::ui::LiveStreamInfo first{QStringLiteral("room-a"), QStringLiteral("uid-a"), QStringLiteral("Alice"), 1};
    ts3stream::ui::LiveStreamInfo second{QStringLiteral("room-b"), QStringLiteral("uid-b"), QStringLiteral("Bob"), 2};
    active.setStreams({second, first});
    check(active.isVisible() && active.text().contains(QStringLiteral("(2)")), "stream count and visibility");
    bool selected = false;
    active.setOnWatch([&](ts3stream::ui::LiveStreamInfo stream) { selected = stream.identity == first.identity; });
    active.menu()->popup(host.mapToGlobal(QPoint(0, 0)));
    QTimer::singleShot(0, &app, [&] {
        active.setStreams({second});
        active.menu()->actions().first()->trigger();
        check(selected, "open stream menu owns its action data across refresh");
        active.setStreams({});
        check(active.isHidden() && !active.menu()->isVisible(), "last stream ended hides button/menu");
        std::printf("viewer audio, LIVE geometry and active streams: failures=%d\n", failures);
        app.exit(failures ? 1 : 0);
    });
    return app.exec();
}
