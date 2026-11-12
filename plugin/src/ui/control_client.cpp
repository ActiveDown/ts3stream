#include "control_client.h"

#include "../log.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include <utility>

namespace ts3stream::ui {
namespace {

QString configFilePath() {
    return QString::fromStdWString(diag::dataDir()) + QStringLiteral("/service.json");
}

QNetworkRequest makeRequest(const ControlConfig& config, const QUrl& url) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    if (!config.clientKey.isEmpty()) {
        request.setRawHeader("X-TS3Stream-Key", config.clientKey.toUtf8());
    }
    request.setTransferTimeout(5000);
    return request;
}

QString replyError(QNetworkReply* reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString body = QString::fromUtf8(reply->readAll()).trimmed();
    if (status > 0) {
        return QStringLiteral("HTTP %1: %2").arg(status).arg(body);
    }
    return reply->errorString();
}

} // namespace

ControlConfig loadControlConfig() {
    ControlConfig config;
    QFile file(configFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return config;
    }
    config.fromFile = true;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject object = document.object();
    const QString url = object.value(QStringLiteral("controlUrl")).toString().trimmed();
    if (!url.isEmpty()) {
        config.baseUrl = url;
    }
    config.clientKey = object.value(QStringLiteral("clientKey")).toString();
    config.autoConfigured = object.value(QStringLiteral("autoConfigured")).toBool(false);
    return config;
}

bool saveControlConfig(const ControlConfig& config) {
    QJsonObject object;
    object.insert(QStringLiteral("controlUrl"), config.baseUrl);
    object.insert(QStringLiteral("clientKey"), config.clientKey);
    if (config.autoConfigured) {
        object.insert(QStringLiteral("autoConfigured"), true);
    }
    QFile file(configFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        diag::log("ControlClient: cannot write service.json");
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    file.close();
    diag::log("ControlClient: saved service.json controlUrl=%s",
              config.baseUrl.toUtf8().constData());
    return true;
}

ControlClient::ControlClient(const ControlConfig& config, QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)), config_(config) {
    diag::log("ControlClient: base=%s auth=%d", config_.baseUrl.toUtf8().constData(),
              config_.clientKey.isEmpty() ? 0 : 1);
}

void ControlClient::fetchToken(const QString& room, const QString& identity, const QString& name,
                               bool publish, TokenCallback callback) {
    QUrl url(config_.baseUrl + QStringLiteral("/api/token"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("room"), room);
    query.addQueryItem(QStringLiteral("identity"), identity);
    query.addQueryItem(QStringLiteral("name"), name);
    query.addQueryItem(QStringLiteral("publish"), publish ? QStringLiteral("1") : QStringLiteral("0"));
    url.setQuery(query);

    QNetworkReply* reply = network_->get(makeRequest(config_, url));
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {}, replyError(reply));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject object = document.object();
        LiveKitToken token;
        token.url = object.value(QStringLiteral("url")).toString();
        token.token = object.value(QStringLiteral("token")).toString();
        token.room = object.value(QStringLiteral("room")).toString();
        if (token.url.isEmpty() || token.token.isEmpty()) {
            callback(false, {}, QStringLiteral("token response has no url/token"));
            return;
        }
        callback(true, token, {});
    });
}

void ControlClient::fetchStreams(const QString& room, StreamsCallback callback) {
    QUrl url(config_.baseUrl + QStringLiteral("/api/streams"));
    if (!room.isEmpty()) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("room"), room);
        url.setQuery(query);
    }

    QNetworkReply* reply = network_->get(makeRequest(config_, url));
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {}, replyError(reply));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray array = document.object().value(QStringLiteral("streams")).toArray();
        QVector<ControlStream> streams;
        streams.reserve(array.size());
        for (const QJsonValue& value : array) {
            const QJsonObject object = value.toObject();
            ControlStream stream;
            stream.room = object.value(QStringLiteral("room")).toString();
            stream.identity = object.value(QStringLiteral("identity")).toString();
            stream.nickname = object.value(QStringLiteral("nickname")).toString();
            stream.channelId = object.value(QStringLiteral("channelId")).toInt();
            stream.serverUid = object.value(QStringLiteral("serverUid")).toString();
            stream.lastSeen = static_cast<qint64>(object.value(QStringLiteral("lastSeen")).toDouble());
            if (!stream.room.isEmpty() && !stream.identity.isEmpty()) {
                streams.push_back(stream);
            }
        }
        callback(true, streams, {});
    });
}

void ControlClient::registerClient(const QString& uid, const QString& nickname,
                                   const QString& serverUid, int channelId,
                                   RegisterCallback callback) {
    QJsonObject body;
    body.insert(QStringLiteral("uid"), uid);
    body.insert(QStringLiteral("nickname"), nickname);
    body.insert(QStringLiteral("serverUid"), serverUid);
    body.insert(QStringLiteral("channelId"), channelId);

    const QUrl url(config_.baseUrl + QStringLiteral("/api/register"));
    QNetworkReply* reply = network_->post(
        makeRequest(config_, url), QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [reply, callback = std::move(callback)] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {}, replyError(reply));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject object = document.object();
        RegisteredConfig config;
        config.controlUrl = object.value(QStringLiteral("controlUrl")).toString();
        config.clientKey = object.value(QStringLiteral("clientKey")).toString();
        config.livekitUrl = object.value(QStringLiteral("livekitUrl")).toString();
        if (config.controlUrl.isEmpty() || config.clientKey.isEmpty()) {
            callback(false, {}, QStringLiteral("register response is missing controlUrl/clientKey"));
            return;
        }
        callback(true, config, {});
    });
}

void ControlClient::announceStart(const QString& room, const QString& identity, const QString& nickname,
                                  int channelId, const QString& serverUid) {
    QJsonObject body;
    body.insert(QStringLiteral("room"), room);
    body.insert(QStringLiteral("identity"), identity);
    body.insert(QStringLiteral("nickname"), nickname);
    body.insert(QStringLiteral("channelId"), channelId);
    body.insert(QStringLiteral("serverUid"), serverUid);
    postJson(QStringLiteral("/api/streams/start"), QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void ControlClient::announceHeartbeat(const QString& room, const QString& identity) {
    QJsonObject body;
    body.insert(QStringLiteral("room"), room);
    body.insert(QStringLiteral("identity"), identity);
    postJson(QStringLiteral("/api/streams/heartbeat"), QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void ControlClient::announceStop(const QString& room, const QString& identity) {
    QJsonObject body;
    body.insert(QStringLiteral("room"), room);
    body.insert(QStringLiteral("identity"), identity);
    postJson(QStringLiteral("/api/streams/stop"), QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void ControlClient::postJson(const QString& path, const QByteArray& body) {
    const QUrl url(config_.baseUrl + path);
    QNetworkReply* reply = network_->post(makeRequest(config_, url), body);
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, path] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            diag::log("ControlClient: %s failed: %s", path.toUtf8().constData(),
                      replyError(reply).toUtf8().constData());
        }
    });
}

} // namespace ts3stream::ui
