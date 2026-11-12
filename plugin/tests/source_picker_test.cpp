#include "ui/source_picker.h"
#include "ui/video_quality.h"
#include "media/window_enumerator.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <cstdio>

// Deterministic sources: this test never captures the desktop or starts a stream.
namespace ts3stream::media {
std::vector<MonitorInfo> enumerateMonitors() {
    return {{reinterpret_cast<HMONITOR>(1), L"Test monitor", {0, 0, 3840, 2160}, true}};
}
std::vector<WindowInfo> enumerateWindows() {
    WindowInfo window;
    window.hwnd = reinterpret_cast<HWND>(2);
    window.title = L"Test window";
    return {window};
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    const int heights[] = {720, 1080, 1440};
    const QSize expected[] = {QSize(1280, 720), QSize(1920, 1080), QSize(2560, 1440)};
    for (int tab = 0; tab < 2; ++tab) {
        for (int preset = 0; preset < 3; ++preset) {
            QTimer::singleShot(0, &app, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) { app.exit(2); return; }
                auto* resolution = dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_resolution"));
                auto* fps = dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_fps"));
                auto* bitrate = dialog->findChild<QSpinBox*>(QStringLiteral("ts3stream_bitrate"));
                auto* tabs = dialog->findChild<QTabWidget*>();
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (!resolution || !fps || !bitrate || !tabs || !buttons) {
                    check(false, "quality controls present");
                    dialog->reject();
                    return;
                }
                check(resolution->currentData().toInt() == 1080, "default is 1080p");
                check(resolution->count() == 3, "three resolution presets");
                check(bitrate->value() == 4000, "default bitrate is 4000 Kbit/s");
                check(bitrate->minimum() == 1000 && bitrate->maximum() == 10000 && bitrate->singleStep() == 1000,
                      "bitrate range and step");
                bitrate->setValue(preset == 0 ? 1000 : preset == 1 ? 5000 : 10000);
                resolution->setCurrentIndex(preset);
                fps->setCurrentIndex(preset);
                tabs->setCurrentIndex(tab);
                buttons->button(QDialogButtonBox::Ok)->click();
            });
            const auto source = ts3stream::ui::pickCaptureSource(nullptr);
            check(source.has_value(), "selection accepted");
            if (!source) continue;
            check(source->resolutionHeight == heights[preset], "selected resolution returned");
            check(source->videoBitrateKbps == (preset == 0 ? 1000 : preset == 1 ? 5000 : 10000),
                  "selected bitrate returned independently of source type");
            check(source->fps == (preset == 0 ? 30 : 60) && source->autoFps == (preset == 2),
                  "FPS selection independent of resolution");
            check(source->type == (tab == 0 ? ts3stream::ui::CaptureSource::Type::Monitor
                                           : ts3stream::ui::CaptureSource::Type::Window), "source type");
            const QSize bounds = ts3stream::ui::videoResolutionBounds(source->resolutionHeight);
            check(bounds == expected[preset], "resolution bounds");
            for (const QSize input : {QSize(3840, 2160), QSize(1080, 1920), QSize(3440, 1440),
                                      QSize(640, 360), QSize(1001, 777), expected[preset]}) {
                QImage image(input, QImage::Format_ARGB32);
                image.fill(Qt::green);
                const QImage output = ts3stream::ui::scaleStreamFrame(image, bounds);
                check(!output.isNull(), "nonempty encoded frame");
                check(output.width() <= bounds.width() && output.height() <= bounds.height(), "within preset bounds");
                check(output.width() <= input.width() && output.height() <= input.height(), "no upscaling");
                const double expectedWidth = double(output.height()) * input.width() / input.height();
                check(qAbs(output.width() - expectedWidth) < 3.0, "aspect ratio preserved within rounding");
                if (input == QSize(3840, 2160)) check(output.size() == expected[preset], "4K downscales to selected preset");
                if (input == QSize(640, 360)) check(output == image, "small window unchanged");
            }
        }
    }
    check(ts3stream::ui::videoResolutionBounds(0) == QSize(1920, 1080), "invalid preset fallback");
    QTimer::singleShot(0, &app, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { check(false, "bitrate dialog present"); return; }
        auto* bitrate = dialog->findChild<QSpinBox*>(QStringLiteral("ts3stream_bitrate"));
        if (!bitrate) { check(false, "bitrate control present"); dialog->reject(); return; }
        bitrate->stepUp();
        check(bitrate->value() == 5000, "step up by 1000");
        bitrate->stepDown();
        check(bitrate->value() == 4000, "step down by 1000");
        auto* editor = bitrate->findChild<QLineEdit*>();
        check(editor != nullptr, "manual bitrate entry present");
        if (editor) {
            editor->setText(QStringLiteral("7000"));
            bitrate->interpretText();
            check(bitrate->value() == 7000, "manually entered bitrate applied");
        }
        bitrate->setValue(0);
        check(bitrate->value() == 1000, "minimum enforced");
        bitrate->setValue(20000);
        check(bitrate->value() == 10000, "maximum enforced");
        dialog->reject();
    });
    check(!ts3stream::ui::pickCaptureSource(nullptr).has_value(), "edited bitrate cancelled without a source");
    QTimer::singleShot(0, &app, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
    });
    check(!ts3stream::ui::pickCaptureSource(nullptr).has_value(), "cancel returns no source");
    std::printf("source picker and resolution scaling: failures=%d\n", failures);
    return failures ? 1 : 0;
}
