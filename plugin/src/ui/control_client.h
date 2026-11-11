#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

class QNetworkAccessManager;

namespace ts3stream::ui {

struct ControlConfig {
    QString baseUrl = QStringLiteral("http://127.0.0.1:8081");
    QString clientKey;
};

struct LiveKitToken {
    QString url;
    QString token;
    QString room;
};

struct ControlStream {
    QString room;
    QString identity;
    QString nickname;
    int channelId = 0;
    QString serverUid;
    qint64 lastSeen = 0;
};

// Reads /plugins/ts3stream/service.json from the TS3 config directory.
// Missing file is fine: defaults point at the local control API.
ControlConfig loadControlConfig();

class ControlClient : public QObject {
public:
    using TokenCallback = std::function<void(bool ok, const LiveKitToken& token, const QString& error)>;
    using StreamsCallback = std::function<void(bool ok, const QVector<ControlStream>& streams, const QString& error)>;

    explicit ControlClient(const ControlConfig& config, QObject* parent = nullptr);

    void fetchToken(const QString& room, const QString& identity, const QString& name,
                    bool publish, TokenCallback callback);
    void fetchStreams(const QString& room, StreamsCallback callback);

    void announceStart(const QString& room, const QString& identity, const QString& nickname,
                       int channelId, const QString& serverUid);
    void announceHeartbeat(const QString& room, const QString& identity);
    void announceStop(const QString& room, const QString& identity);

private:
    void postJson(const QString& path, const QByteArray& body);
    QNetworkAccessManager* network_ = nullptr;
    ControlConfig config_;
};

} // namespace ts3stream::ui
