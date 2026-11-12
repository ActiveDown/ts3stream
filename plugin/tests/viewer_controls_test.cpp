#include "media/playback_gain.h"
#include "ui/stream_panel.h"
#include "ui/active_streams_button.h"
#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QLabel>
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
    const int16_t boosted[] = {-32768, -1851, -1, 0, 1, 1851, 32767};
    for (int i = 0; i < 7; ++i) {
        check(ts3stream::media::applyPlaybackGain(samples[i], 0) == 0, "mute must output silence");
        check(ts3stream::media::applyPlaybackGain(samples[i], 50) == half[i], "50% PCM gain");
        check(ts3stream::media::applyPlaybackGain(samples[i], 100) == samples[i], "100% PCM unchanged");
        check(ts3stream::media::applyPlaybackGain(samples[i], -10) == 0, "lower clamp");
        check(ts3stream::media::applyPlaybackGain(samples[i], 150) == boosted[i], "150% PCM gain and saturation");
        check(ts3stream::media::applyPlaybackGain(samples[i], 200) == boosted[i], "upper clamp to 150%");
    }
    check(ts3stream::media::applyPlaybackGain(26000, 125) == 32500, "positive gain below clipping");
    check(ts3stream::media::applyPlaybackGain(-26000, 125) == -32500, "negative gain below clipping");
    check(ts3stream::media::applyPlaybackGain(27000, 125) == 32767, "positive peak saturates");
    check(ts3stream::media::applyPlaybackGain(-27000, 125) == -32768, "negative peak saturates");
    check(ts3stream::media::applyPlaybackGain(21000, 150) == 31500, "150% positive below clipping");
    check(ts3stream::media::applyPlaybackGain(-21000, 150) == -31500, "150% negative below clipping");
    check(ts3stream::media::applyPlaybackGain(22000, 150) == 32767, "150% positive saturation");
    check(ts3stream::media::applyPlaybackGain(-22000, 150) == -32768, "150% negative saturation");
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
    check(slider->minimum() == 0 && slider->maximum() == 150 && slider->value() == 100,
          "volume range is 0-150%, default remains 100%");
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
    slider->setValue(150);
    check(volume == 150 && !muted && notifications == 4, "boost reaches playback callback");
    bool boostLabel = false;
    for (auto* label : controls->findChildren<QLabel*>()) {
        boostLabel = boostLabel || label->text() == QStringLiteral("150%");
    }
    check(boostLabel, "boost label displays 150%");
    mute->click();
    check(volume == 150 && muted && notifications == 5, "mute preserves boosted volume");
    panel.setFullscreenMode(true);
    mute->click();
    check(volume == 150 && !muted && notifications == 6, "fullscreen unmute restores boost");
    panel.setFullscreenMode(false);
    check(slider->value() == 150, "boost survives fullscreen round trip");
    panel.setPlaybackAudio(999, false);
    check(slider->value() == 150 && notifications == 6, "programmatic upper clamp without feedback");
    panel.setPlaybackAudio(-10, false);
    check(slider->value() == 0 && notifications == 6, "programmatic lower clamp without feedback");

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
