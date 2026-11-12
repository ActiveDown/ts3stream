#include "stream_preferences.h"
#include "../log.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace ts3stream::ui {
namespace {
QString preferencesPath() {
    return QString::fromStdWString(diag::dataDir()) + QStringLiteral("/stream-settings.json");
}

bool validResolution(int height) {
    return height == 720 || height == 900 || height == 1080 || height == 1440;
}
} // namespace

void loadStreamPreferences(CaptureSource& source) {
    // Validate each field independently. Missing/corrupt files use first-run defaults.
    source.resolutionHeight = 1080;
    source.fps = 30;
    source.autoFps = false;
    source.videoBitrateKbps = media::kDefaultVideoBitrateKbps;
    QFile file(preferencesPath());
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const int height = object.value(QStringLiteral("resolutionHeight")).toInt();
    if (validResolution(height)) source.resolutionHeight = height;
    const QString mode = object.value(QStringLiteral("fpsMode")).toString();
    if (mode == QStringLiteral("60") || mode == QStringLiteral("auto")) {
        source.fps = 60;
        source.autoFps = mode == QStringLiteral("auto");
    }
    const int bitrate = object.value(QStringLiteral("videoBitrateKbps")).toInt();
    if (bitrate >= media::kMinVideoBitrateKbps && bitrate <= media::kMaxVideoBitrateKbps) {
        source.videoBitrateKbps = bitrate;
    }
}

bool saveStreamPreferences(const CaptureSource& source) {
    if (!validResolution(source.resolutionHeight) ||
        (source.fps != 30 && source.fps != 60) ||
        source.videoBitrateKbps < media::kMinVideoBitrateKbps ||
        source.videoBitrateKbps > media::kMaxVideoBitrateKbps) return false;
    QJsonObject object;
    object.insert(QStringLiteral("resolutionHeight"), source.resolutionHeight);
    object.insert(QStringLiteral("fpsMode"), source.autoFps ? QStringLiteral("auto") : QString::number(source.fps));
    object.insert(QStringLiteral("videoBitrateKbps"), source.videoBitrateKbps);
    if (!QDir().mkpath(QString::fromStdWString(diag::dataDir()))) return false;
    QSaveFile file(preferencesPath());
    if (!file.open(QIODevice::WriteOnly)) return false;
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) return false;
    // Commit immediately and atomically, rather than waiting for TS3 to exit.
    return file.commit();
}

} // namespace ts3stream::ui
