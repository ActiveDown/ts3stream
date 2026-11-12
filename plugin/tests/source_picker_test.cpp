#include "ui/source_picker.h"
#include "ui/video_quality.h"
#include "ui/stream_preferences.h"
#include "media/window_enumerator.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

// Never read or write the user's actual plugin configuration.
static QString testDataDir;
namespace ts3stream::diag {
std::wstring dataDir() { return testDataDir.toStdWString(); }
}

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
    const auto args = app.arguments();
    if (args.size() == 6 && args[1] == QStringLiteral("--restore")) {
        testDataDir = args[2];
        QTimer::singleShot(0, &app, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) { app.exit(2); return; }
            auto* resolution = dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_resolution"));
            auto* fps = dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_fps"));
            auto* bitrate = dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_bitrate"));
            check(resolution->currentData().toInt() == args[3].toInt(), "fresh process restores resolution");
            check(fps->currentIndex() == args[4].toInt(), "fresh process restores distinct FPS mode");
            check(bitrate->currentText().toInt() == args[5].toInt(), "fresh process restores bitrate");
            if (args[5].toInt() % 1000 == 0) {
                check(bitrate->currentIndex() == args[5].toInt() / 1000 - 1, "restored preset selected in dropdown");
            }
            dialog->reject();
        });
        ts3stream::ui::pickCaptureSource(nullptr);
        return failures ? 1 : 0;
    }
    QTemporaryDir temp;
    check(temp.isValid(), "isolated temporary profile");
    if (!temp.isValid()) return 1;
    testDataDir = temp.path() + QStringLiteral("/nested/профиль");
    const QString path = testDataDir + QStringLiteral("/stream-settings.json");
    const int heights[] = {720, 900, 1080, 1440};
    const QSize expected[] = {QSize(1280, 720), QSize(1600, 900), QSize(1920, 1080), QSize(2560, 1440)};
    for (int tab = 0; tab < 2; ++tab) {
        for (int preset = 0; preset < 4; ++preset) {
            QTimer::singleShot(0, &app, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) { app.exit(2); return; }
                auto* resolution = dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_resolution"));
                auto* fps = dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_fps"));
                auto* bitrate = dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_bitrate"));
                auto* tabs = dialog->findChild<QTabWidget*>();
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (!resolution || !fps || !bitrate || !tabs || !buttons) {
                    check(false, "quality controls present");
                    dialog->reject();
                    return;
                }
                check(resolution->currentData().toInt() == 1080, "default is 1080p");
                check(resolution->count() == 4, "four resolution presets including 900p");
                check(bitrate->currentText() == QStringLiteral("4000"), "default bitrate is 4000 Kbit/s");
                check(bitrate->isEditable() && bitrate->count() == 10, "editable bitrate dropdown");
                for (int i = 0; i < 10; ++i) {
                    check(bitrate->itemText(i).toInt() == (i + 1) * 1000, "1000 through 10000 presets");
                }
                bitrate->setCurrentIndex(preset == 0 ? 0 : preset == 1 ? 4 : 9);
                resolution->setCurrentIndex(preset);
                fps->setCurrentIndex(preset % 3);
                tabs->setCurrentIndex(tab);
                buttons->button(QDialogButtonBox::Ok)->click();
            });
            const auto source = ts3stream::ui::pickCaptureSource(nullptr);
            check(source.has_value(), "selection accepted");
            if (!source) continue;
            check(source->resolutionHeight == heights[preset], "selected resolution returned");
            check(source->videoBitrateKbps == (preset == 0 ? 1000 : preset == 1 ? 5000 : 10000),
                  "selected bitrate returned independently of source type");
            check(source->fps == (preset % 3 == 0 ? 30 : 60) && source->autoFps == (preset % 3 == 2),
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
    check(!QFile::exists(path), "opening/accepting picker does not save before capture starts");
    check(ts3stream::ui::videoResolutionBounds(0) == QSize(1920, 1080), "invalid preset fallback");
    QTimer::singleShot(0, &app, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { check(false, "bitrate dialog present"); return; }
        auto* bitrate = dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_bitrate"));
        if (!bitrate) { check(false, "bitrate control present"); dialog->reject(); return; }
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        auto* editor = bitrate->lineEdit();
        check(editor != nullptr, "manual bitrate entry present");
        if (editor) {
            for (const auto& text : {QString(), QStringLiteral("999"), QStringLiteral("10001"),
                                    QStringLiteral("-5000"), QStringLiteral("5.5"), QStringLiteral("abc")}) {
                editor->setText(text);
                check(!editor->hasAcceptableInput(), "invalid bitrate rejected");
                check(!buttons->button(QDialogButtonBox::Ok)->isEnabled(), "invalid input disables start");
            }
            editor->setText(QStringLiteral("5500"));
            check(editor->hasAcceptableInput(), "custom bitrate accepted");
            check(buttons->button(QDialogButtonBox::Ok)->isEnabled(), "valid input enables start");
        }
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    const auto custom = ts3stream::ui::pickCaptureSource(nullptr);
    check(custom && custom->videoBitrateKbps == 5500, "custom bitrate returned");

    auto readFile = [&] {
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        return file.readAll();
    };
    auto restoreInNewProcess = [&](int height, int mode, int bitrate) {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--restore"), testDataDir, QString::number(height),
                     QString::number(mode), QString::number(bitrate)});
        const bool finished = child.waitForFinished(15000);
        check(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
              "preferences survive process restart and populate picker");
        if (!finished) { child.kill(); child.waitForFinished(); }
        if (child.exitCode() != 0) std::printf("%s\n", child.readAllStandardOutput().constData());
    };
    for (int mode = 0; mode < 3; ++mode) {
        ts3stream::ui::CaptureSource source;
        source.resolutionHeight = 900;
        source.fps = mode == 0 ? 30 : 60;
        source.autoFps = mode == 2;
        source.videoBitrateKbps = mode == 0 ? 5000 : 5500;
        check(ts3stream::ui::saveStreamPreferences(source), "save immediately creates config directory/file");
        check(QJsonDocument::fromJson(readFile()).isObject(), "valid on-disk JSON");
        restoreInNewProcess(900, mode, source.videoBitrateKbps);
    }
    const auto saved = readFile();
    QTimer::singleShot(0, &app, [&] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_resolution"))->setCurrentIndex(0);
            dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_fps"))->setCurrentIndex(0);
            dialog->findChild<QComboBox*>(QStringLiteral("ts3stream_bitrate"))->setCurrentIndex(0);
            dialog->reject();
        }
    });
    check(!ts3stream::ui::pickCaptureSource(nullptr).has_value(), "cancel returns no source");
    check(readFile() == saved, "cancel preserves previous settings byte-for-byte");
    restoreInNewProcess(900, 2, 5500);
    ts3stream::ui::CaptureSource invalid;
    invalid.videoBitrateKbps = 10001;
    check(!ts3stream::ui::saveStreamPreferences(invalid), "invalid save refused");
    check(readFile() == saved, "invalid save preserves file");
    auto writeFile = [&](const QByteArray& data) {
        QFile file(path);
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open fixture");
        check(file.write(data) == data.size(), "write fixture");
    };
    for (const auto& data : {QByteArray("{broken"), QByteArray("[]"), QByteArray("{}"),
                            QByteArray("{\"resolutionHeight\":1,\"fpsMode\":\"bad\",\"videoBitrateKbps\":10001}"),
                            QByteArray("{\"resolutionHeight\":\"900\",\"fpsMode\":60,\"videoBitrateKbps\":-1}")}) {
        writeFile(data);
        restoreInNewProcess(1080, 0, 4000);
    }
    writeFile("{\"resolutionHeight\":900,\"fpsMode\":\"auto\",\"videoBitrateKbps\":999}");
    restoreInNewProcess(900, 2, 4000);
    check(QFile::remove(path), "remove fixture");
    check(QDir().mkdir(path), "block file path with directory");
    check(!ts3stream::ui::saveStreamPreferences(ts3stream::ui::CaptureSource{}), "write failure reported");
    std::printf("source picker and resolution scaling: failures=%d\n", failures);
    return failures ? 1 : 0;
}
