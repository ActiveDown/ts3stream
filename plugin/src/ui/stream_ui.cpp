#include "stream_ui.h"

#include "stream_panel.h"
#include "../diag/widget_dump.h"
#include "../log.h"
#include "../media/window_enumerator.h"

#include <QAction>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QShortcut>
#include <QFile>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QWidget>
#include <QWidgetAction>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>

namespace ts3stream::ui {
namespace {

QIcon makeStreamIcon() {
    QPixmap pixmap(28, 28);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.setPen(QPen(QColor(220, 220, 220), 1.8));
    painter.setBrush(QColor(60, 60, 60));
    painter.drawRoundedRect(QRectF(3, 5, 22, 15), 2.5, 2.5);
    painter.drawLine(QPointF(10, 23), QPointF(18, 23));
    painter.drawLine(QPointF(14, 20), QPointF(14, 23));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(220, 45, 45));
    painter.drawEllipse(QRectF(17, 15, 9, 9));
    painter.end();
    return QIcon(pixmap);
}

QString sourceSubtitle(const CaptureSource& source) {
    if (source.type == CaptureSource::Type::Window) {
        return source.processName.isEmpty() ? QStringLiteral("Окно приложения") : source.processName;
    }
    return QStringLiteral("Захват экрана");
}

struct LiveKitConfig {
    QString url;
    QString token;
};

std::optional<LiveKitConfig> loadLiveKitConfig() {
    const QString path = QString::fromStdWString(diag::dataDir()) + QStringLiteral("/livekit.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return std::nullopt;
    }

    const QJsonObject object = document.object();
    LiveKitConfig config;
    config.url = object.value(QStringLiteral("url")).toString();
    config.token = object.value(QStringLiteral("token")).toString();
    if (config.url.isEmpty() || config.token.isEmpty()) {
        return std::nullopt;
    }
    return config;
}

} // namespace

StreamUi& StreamUi::instance() {
    // Intentionally leaked: Qt destroys the application and the plugin loader
    // unloads DLLs during TS3 shutdown. Running a static QObject destructor at
    // that point causes crashes.
    static StreamUi* ui = new StreamUi();
    return *ui;
}

void StreamUi::setTs3Functions(const TS3Functions& functions) {
    ts3_ = functions;
}

bool StreamUi::roomForChannel(uint64 serverConnectionHandlerID, int channelId,
                              QString& room, QString& serverUid) const {
    if (!ts3_.getServerVariableAsString || !ts3_.freeMemory || channelId <= 0) {
        return false;
    }
    if (serverConnectionHandlerID == 0) {
        serverConnectionHandlerID = ts3_.getCurrentServerConnectionHandlerID
                                        ? ts3_.getCurrentServerConnectionHandlerID()
                                        : 0;
    }
    if (serverConnectionHandlerID == 0) {
        return false;
    }
    char* value = nullptr;
    if (ts3_.getServerVariableAsString(serverConnectionHandlerID, VIRTUALSERVER_UNIQUE_IDENTIFIER,
                                       &value) != 0 || !value) {
        return false;
    }
    serverUid = QString::fromUtf8(value);
    ts3_.freeMemory(value);
    if (serverUid.isEmpty()) {
        return false;
    }
    const QString serverHash = QString::fromLatin1(
        QCryptographicHash::hash(serverUid.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    room = QStringLiteral("ts3-%1-%2").arg(serverHash).arg(channelId);
    return true;
}

void StreamUi::openStreamForClient(uint64 serverConnectionHandlerID, anyID clientID) {
    if (!control_ || clientID == 0) {
        return;
    }
    if (!ts3_.getClientVariableAsString || !ts3_.getChannelOfClient || !ts3_.freeMemory) {
        return;
    }

    char* nameValue = nullptr;
    if (ts3_.getClientVariableAsString(serverConnectionHandlerID, clientID, CLIENT_NICKNAME, &nameValue) != 0 ||
        !nameValue) {
        return;
    }
    const QString nickname = QString::fromUtf8(nameValue);
    ts3_.freeMemory(nameValue);

    char* uidValue = nullptr;
    if (ts3_.getClientVariableAsString(serverConnectionHandlerID, clientID, CLIENT_UNIQUE_IDENTIFIER, &uidValue) != 0 ||
        !uidValue) {
        return;
    }
    const QString uniqueId = QString::fromUtf8(uidValue);
    ts3_.freeMemory(uidValue);

    uint64 channelId = 0;
    if (ts3_.getChannelOfClient(serverConnectionHandlerID, clientID, &channelId) != 0 || channelId == 0) {
        return;
    }

    QString room;
    QString serverUid;
    if (!roomForChannel(serverConnectionHandlerID, static_cast<int>(channelId), room, serverUid)) {
        return;
    }

    diag::log("StreamUi: context menu stream request client=%s room=%s",
              nickname.toUtf8().constData(), room.toUtf8().constData());
    control_->fetchStreams(room, [this, room, nickname, uniqueId](
                                        bool ok, const QVector<ControlStream>& streams, const QString& error) {
        if (!initialized_) {
            return;
        }
        if (!ok) {
            diag::log("StreamUi: context menu stream query failed: %s", error.toUtf8().constData());
            return;
        }
        for (const ControlStream& stream : streams) {
            if (stream.identity == uniqueId || stream.nickname == nickname) {
                startViewerForRoom(room, stream.identity, stream.nickname);
                return;
            }
        }
        diag::log("StreamUi: client %s is not streaming", nickname.toUtf8().constData());
    });
}

void StreamUi::ensureWatchButton() {
    if (!infoFrame_) {
        return;
    }
    QWidget* parent = infoFrame_.data();
    if (auto* area = qobject_cast<QAbstractScrollArea*>(infoFrame_)) {
        parent = area->viewport();
    }
    if (!parent) {
        parent = infoFrame_.data();
    }
    if (watchButton_ && watchButton_->parentWidget() != parent) {
        watchButton_->setParent(parent);
    }
    if (!watchButton_) {
        watchButton_ = new QToolButton(parent);
        watchButton_->setObjectName("ts3stream_watch_button");
        watchButton_->setText(QStringLiteral("▶  Смотреть стрим"));
        watchButton_->setToolTip(QStringLiteral("Открыть трансляцию выбранного пользователя"));
        watchButton_->setCursor(Qt::PointingHandCursor);
        watchButton_->setMinimumSize(220, 38);
        watchButton_->setStyleSheet(
            "QToolButton { color: white; background: #d93025; border: 1px solid #ff6b60;"
            " border-radius: 6px; padding: 6px 14px; font-size: 13px; font-weight: 700; }"
            "QToolButton:hover { background: #ef4a3f; }");
        watchButton_->hide();
        QObject::connect(watchButton_, &QToolButton::clicked, this, [this] {
            diag::log("StreamUi: watch button clicked room=%s nick=%s",
                      watchStream_.room.toUtf8().constData(),
                      watchStream_.nickname.toUtf8().constData());
            if (!watchStream_.room.isEmpty()) {
                startViewerForRoom(watchStream_.room, watchStream_.identity, watchStream_.nickname);
            }
        });
    }
    watchButton_->raise();
}

void StreamUi::updateWatchButton() {
    if (!infoFrame_ || !treeView_ || !treeView_->selectionModel()) {
        if (watchButton_) {
            watchButton_->hide();
        }
        return;
    }
    ensureWatchButton();
    if (!watchButton_) {
        return;
    }
    if (panel_) {
        watchButton_->hide();
        return;
    }

    const QModelIndex current = treeView_->currentIndex();
    if (!current.isValid()) {
        watchButton_->hide();
        return;
    }
    const QString nickname = current.data(Qt::DisplayRole).toString().trimmed();
    const auto it = liveStreams_.constFind(nickname);
    if (it == liveStreams_.constEnd()) {
        watchButton_->hide();
        return;
    }

    watchStream_ = it.value();
    ensureWatchButton();
    if (!watchButton_) {
        return;
    }
    QWidget* parent = watchButton_->parentWidget();
    if (!parent) {
        return;
    }
    const int width = qMin(240, qMax(160, parent->width() - 48));
    const int height = 38;
    const int x = (parent->width() - width) / 2;
    const int y = parent->height() / 2 + 30;
    watchButton_->setGeometry(x, y, width, height);
    watchButton_->show();
    watchButton_->raise();
}

bool StreamUi::roomContext(QString& room, QString& serverUid, int& channelId,
                           QString& identity, QString& nickname) const {
    if (!ts3_.getCurrentServerConnectionHandlerID || !ts3_.getClientID ||
        !ts3_.getChannelOfClient || !ts3_.getClientVariableAsString ||
        !ts3_.getServerVariableAsString || !ts3_.freeMemory) {
        return false;
    }

    const uint64 scHandler = ts3_.getCurrentServerConnectionHandlerID();
    if (scHandler == 0) {
        return false;
    }

    anyID selfId = 0;
    if (ts3_.getClientID(scHandler, &selfId) != 0 || selfId == 0) {
        return false;
    }

    uint64 tsChannelId = 0;
    if (ts3_.getChannelOfClient(scHandler, selfId, &tsChannelId) != 0 || tsChannelId == 0) {
        return false;
    }

    auto queryClient = [&](size_t flag) -> QString {
        char* value = nullptr;
        if (ts3_.getClientVariableAsString(scHandler, selfId, flag, &value) != 0 || !value) {
            return {};
        }
        const QString result = QString::fromUtf8(value);
        ts3_.freeMemory(value);
        return result;
    };
    auto queryServer = [&](size_t flag) -> QString {
        char* value = nullptr;
        if (ts3_.getServerVariableAsString(scHandler, flag, &value) != 0 || !value) {
            return {};
        }
        const QString result = QString::fromUtf8(value);
        ts3_.freeMemory(value);
        return result;
    };

    identity = queryClient(CLIENT_UNIQUE_IDENTIFIER);
    nickname = queryClient(CLIENT_NICKNAME);
    serverUid = queryServer(VIRTUALSERVER_UNIQUE_IDENTIFIER);
    if (identity.isEmpty() || nickname.isEmpty() || serverUid.isEmpty()) {
        return false;
    }

    // One LiveKit room per TS3 virtual server + channel. Hashing keeps the
    // room name short and avoids base64 characters from the server UID.
    const QString serverHash = QString::fromLatin1(
        QCryptographicHash::hash(serverUid.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    room = QStringLiteral("ts3-%1-%2").arg(serverHash).arg(static_cast<qulonglong>(tsChannelId));
    channelId = static_cast<int>(tsChannelId);
    return true;
}

void StreamUi::onTalkStatusChanged(uint64 serverConnectionHandlerID, anyID clientID, bool talking) {
    (void)serverConnectionHandlerID;
    std::lock_guard<std::mutex> lock(talkMutex_);
    if (talking) {
        talkingClients_.insert(clientID);
    } else {
        talkingClients_.erase(clientID);
    }
}

void StreamUi::updateRoomMembers() {
    if (!initialized_ || !panel_) {
        return;
    }

    std::vector<RoomMember> members;
    if (ts3_.getCurrentServerConnectionHandlerID && ts3_.getChannelClientList &&
        ts3_.getClientVariableAsString && ts3_.freeMemory) {
        const uint64 scHandler = ts3_.getCurrentServerConnectionHandlerID();
        if (scHandler != 0) {
            anyID selfId = 0;
            if (ts3_.getClientID) {
                ts3_.getClientID(scHandler, &selfId);
            }
            uint64 channelId = 0;
            if (ts3_.getChannelOfClient) {
                ts3_.getChannelOfClient(scHandler, selfId, &channelId);
            }

            anyID* clients = nullptr;
            if (ts3_.getChannelClientList(scHandler, channelId, &clients) == 0 && clients) {
                std::lock_guard<std::mutex> lock(talkMutex_);
                for (anyID* current = clients; *current != 0; ++current) {
                    const anyID id = *current;
                    char* name = nullptr;
                    if (ts3_.getClientVariableAsString(scHandler, id, CLIENT_NICKNAME, &name) == 0 && name) {
                        RoomMember member;
                        member.name = QString::fromUtf8(name);
                        member.self = (id == selfId);
                        member.talking = talkingClients_.count(id) > 0;
                        members.push_back(member);
                        ts3_.freeMemory(name);
                    }
                }
                ts3_.freeMemory(clients);
            }
        }
    }

    panel_->setMembers(members);
}

void StreamUi::installLiveBadgeDelegate() {
    QTreeView* tree = qobject_cast<QTreeView*>(serverTree_.data());
    if (!tree) {
        serverTree_ = findVisibleServerTree(mainWindow_);
        tree = qobject_cast<QTreeView*>(serverTree_.data());
    }
    if (!tree) {
        return;
    }

    if (tree != treeView_) {
        if (treeViewport_) {
            treeViewport_->removeEventFilter(this);
        }
        treeView_ = tree;
        treeViewport_ = tree->viewport();
        if (treeViewport_) {
            treeViewport_->installEventFilter(this);
        }
        liveDelegate_ = nullptr;
    }

    if (liveDelegate_ && tree->itemDelegate() == liveDelegate_) {
        return;
    }

    QAbstractItemDelegate* inner = tree->itemDelegate();
    if (!inner) {
        return;
    }
    if (liveDelegate_) {
        liveDelegate_->deleteLater();
    }
    auto* delegate = new LiveBadgeDelegate(inner, tree);
    delegate->setStreams(liveStreams_);
    tree->setItemDelegate(delegate);
    liveDelegate_ = delegate;
    diag::log("StreamUi: LIVE badge delegate installed");
}

void StreamUi::refreshLiveStreams() {
    if (!initialized_ || !control_ || !connected_ || streamsRequestActive_) {
        return;
    }
    QString room;
    QString serverUid;
    QString identity;
    QString nickname;
    int channelId = 0;
    if (!roomContext(room, serverUid, channelId, identity, nickname)) {
        return;
    }

    streamsRequestActive_ = true;
    control_->fetchStreams(room, [this](bool ok, const QVector<ControlStream>& streams, const QString& error) {
        streamsRequestActive_ = false;
        if (!initialized_) {
            return;
        }
        if (!ok) {
            diag::log("StreamUi: live stream query failed: %s", error.toUtf8().constData());
            return;
        }

        QHash<QString, LiveStreamInfo> byNickname;
        for (const ControlStream& stream : streams) {
            if (stream.nickname.isEmpty()) {
                continue;
            }
            LiveStreamInfo info;
            info.room = stream.room;
            info.identity = stream.identity;
            info.nickname = stream.nickname;
            info.channelId = stream.channelId;
            byNickname.insert(stream.nickname, info);
        }
        liveStreams_ = byNickname;
        if (liveDelegate_) {
            liveDelegate_->setStreams(liveStreams_);
        }
        if (treeView_) {
            treeView_->viewport()->update();
        }
        updateWatchButton();
    });
}

QWidget* StreamUi::findMainWindow() const {
    QWidget* best = nullptr;
    qint64 bestArea = 0;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (!widget || !widget->isWindow() || !widget->isVisible()) {
            continue;
        }
        const QString title = widget->windowTitle();
        if (title.contains("TeamSpeak", Qt::CaseInsensitive)) {
            return widget;
        }
        const qint64 area = static_cast<qint64>(widget->width()) * widget->height();
        if (area > bestArea) {
            bestArea = area;
            best = widget;
        }
    }
    return best;
}

QWidget* StreamUi::findVisibleInfoFrame(QWidget* mainWindow) const {
    if (!mainWindow) {
        return nullptr;
    }
    const auto frames = mainWindow->findChildren<QWidget*>(QStringLiteral("InfoFrame"));
    for (QWidget* frame : frames) {
        if (frame && frame->isVisible()) {
            return frame;
        }
    }
    return frames.isEmpty() ? nullptr : frames.first();
}

QWidget* StreamUi::findVisibleServerTree(QWidget* mainWindow) const {
    if (!mainWindow) {
        return nullptr;
    }
    const auto trees = mainWindow->findChildren<QWidget*>(QStringLiteral("ServerTreeView"));
    for (QWidget* tree : trees) {
        if (tree && tree->isVisible()) {
            return tree;
        }
    }
    return trees.isEmpty() ? nullptr : trees.first();
}

void StreamUi::initialize() {
    if (initialized_) {
        return;
    }

    mainWindow_ = findMainWindow();
    if (!mainWindow_) {
        diag::log("StreamUi: main window not found");
        return;
    }
    if (!qEnvironmentVariableIsEmpty("TS3STREAM_MINIMAL_INIT")) {
        initialized_ = true;
        diag::log("StreamUi: minimal init (no widgets touched)");
        return;
    }

    control_ = std::make_unique<ControlClient>(loadControlConfig(), this);

    toolBar_ = mainWindow_->findChild<QToolBar*>();
    if (!toolBar_) {
        diag::log("StreamUi: toolbar not found");
        return;
    }

    // Never install an event filter on TS3-owned widgets: the client tears them
    // down outside Qt's normal object lifetime and the filter callback later
    // fails inside ucrtbase. Geometry is kept in sync by the 100 ms watchdog.
    infoFrame_ = findVisibleInfoFrame(mainWindow_);
    if (infoFrame_) {
        infoWrapper_ = infoFrame_->parentWidget();
    }
    serverTree_ = findVisibleServerTree(mainWindow_);

    if (qEnvironmentVariableIsEmpty("TS3STREAM_DISABLE_BUTTON")) {
    streamButton_ = new QToolButton(toolBar_);
    streamButton_->setObjectName("ts3stream_start_button");
    streamButton_->setText(QStringLiteral("Стрим"));
    streamButton_->setIcon(makeStreamIcon());
    streamButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    streamButton_->setToolTip(QStringLiteral("TS3Stream — начать стрим экрана или приложения"));
    streamButton_->setAutoRaise(true);
    streamButton_->setCursor(Qt::PointingHandCursor);
    streamButton_->setPopupMode(QToolButton::InstantPopup);
    streamButton_->setEnabled(false);
    streamButton_->setToolTip(QStringLiteral("Подключись к серверу TS3, чтобы начать стрим"));

    streamMenu_ = new QMenu(streamButton_);
    QAction* startAction = streamMenu_->addAction(QStringLiteral("Начать стрим…"));
    QAction* stopAction = streamMenu_->addAction(QStringLiteral("Остановить"));
    startAction_ = startAction;
    stopAction_ = stopAction;
    stopAction_->setEnabled(false);
    streamMenu_->addSeparator();
    QAction* dumpAction = streamMenu_->addAction(QStringLiteral("Диагностика: дамп виджетов"));

    streamButton_->setMenu(streamMenu_);

    // Insert before the toolbar stretcher, otherwise the stretcher consumes all
    // remaining width and the toolbar pushes the button into its overflow menu.
    QAction* beforeAction = nullptr;
    if (QWidget* stretcher = toolBar_->findChild<QWidget*>(QStringLiteral("ToolbarStretcher"))) {
        for (QAction* action : toolBar_->actions()) {
            if (auto* widgetAction = qobject_cast<QWidgetAction*>(action)) {
                if (widgetAction->defaultWidget() == stretcher) {
                    beforeAction = action;
                    break;
                }
            }
        }
    }

    if (qEnvironmentVariableIsEmpty("TS3STREAM_DISABLE_BUTTON")) {
        if (beforeAction) {
            toolBar_->insertWidget(beforeAction, streamButton_);
            diag::log("StreamUi: button inserted before ToolbarStretcher");
        } else {
            toolBar_->addWidget(streamButton_);
            diag::log("StreamUi: button appended to toolbar");
        }
        streamButton_->show();
    }
    diag::log("StreamUi: button added geometry=%d,%d %dx%d visible=%d toolbarActions=%lld",
              streamButton_->x(), streamButton_->y(), streamButton_->width(), streamButton_->height(),
              streamButton_->isVisible() ? 1 : 0,
              static_cast<long long>(toolBar_->actions().size()));

    QObject::connect(startAction, &QAction::triggered, this, [this] { startFromPicker(); });
    QObject::connect(stopAction, &QAction::triggered, this, [this] { stopCapture(); });
    QObject::connect(dumpAction, &QAction::triggered, this, [this] {
        const QString dir = QString::fromStdString(
            diag::toUtf8(diag::dataDir())) + "/plugins/ts3stream";
        QDir().mkpath(dir);
        diag::dumpWidgetTree(dir + "/widgets.txt");
    });
    } else {
        diag::log("StreamUi: toolbar button disabled by TS3STREAM_DISABLE_BUTTON");
    }

    if (qEnvironmentVariableIsEmpty("TS3STREAM_DISABLE_TIMERS")) {
        watchdog_ = new QTimer(this);
        watchdog_->setInterval(100);
        QObject::connect(watchdog_, &QTimer::timeout, this, [this] {
            if (!initialized_) {
                return;
            }
            if (qEnvironmentVariableIsEmpty("TS3STREAM_DISABLE_GATE")) {
                updateStreamButtonState();
            }
            QWidget* current = findVisibleInfoFrame(mainWindow_);
            if (current && current != infoFrame_) {
                diag::log("StreamUi: active InfoFrame changed, migrating");
                infoFrame_ = current;
                infoWrapper_ = infoFrame_->parentWidget();
                if (panel_) {
                    ensurePanel();
                }
            }
            refreshPanelGeometry();
            updateWatchButton();
        });
        watchdog_->start();

        membersTimer_ = new QTimer(this);
        membersTimer_->setInterval(700);
        QObject::connect(membersTimer_, &QTimer::timeout, this, [this] { updateRoomMembers(); });
        membersTimer_->start();

        streamsTimer_ = new QTimer(this);
        streamsTimer_->setInterval(2500);
        QObject::connect(streamsTimer_, &QTimer::timeout, this, [this] { refreshLiveStreams(); });
        streamsTimer_->start();
    }

    installLiveBadgeDelegate();
    ensureWatchButton();

    auto* fullscreenShortcut = new QShortcut(QKeySequence(Qt::Key_F11), mainWindow_);
    QObject::connect(fullscreenShortcut, &QShortcut::activated, this, [this] { toggleFullscreen(); });

    initialized_ = true;
    diag::log("StreamUi initialized: toolbar=%s infoFrame=%s serverTree=%s",
              toolBar_->metaObject()->className(),
              infoFrame_ ? infoFrame_->metaObject()->className() : "null",
              serverTree_ ? serverTree_->metaObject()->className() : "null");

    QString autostart = qEnvironmentVariable("TS3STREAM_AUTOSTART");
    const QString autostartFile = QString::fromStdWString(diag::dataDir()) + "/autostart.txt";
    {
        QFile file(autostartFile);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            autostart = QString::fromUtf8(file.readAll()).trimmed();
            file.close();
            QFile::remove(autostartFile);
        }
    }
    diag::log("StreamUi: autostart=\"%s\"", autostart.toUtf8().constData());
    if (autostart == QStringLiteral("monitor")) {
        auto* autostartTimer = new QTimer(this);
        autostartTimer->setInterval(500);
        QObject::connect(autostartTimer, &QTimer::timeout, this, [this, autostartTimer] {
            if (!initialized_) {
                autostartTimer->stop();
                autostartTimer->deleteLater();
                return;
            }
            if (!connected_) {
                return;
            }
            autostartTimer->stop();
            autostartTimer->deleteLater();
            const auto monitors = media::enumerateMonitors();
            if (monitors.empty()) {
                diag::log("autostart: no monitors");
                return;
            }
            const auto primary = std::find_if(monitors.begin(), monitors.end(),
                                              [](const media::MonitorInfo& monitor) { return monitor.primary; });
            const auto& monitor = primary != monitors.end() ? *primary : monitors.front();
            const int width = monitor.rect.right - monitor.rect.left;
            const int height = monitor.rect.bottom - monitor.rect.top;
            CaptureSource source;
            source.type = CaptureSource::Type::Monitor;
            source.monitor = monitor.handle;
            source.title = QStringLiteral("Экран — %1×%2").arg(width).arg(height);
            source.audioType = CaptureSource::AudioType::Device;
            source.audioDeviceName = QStringLiteral("устройство по умолчанию");
            diag::log("autostart: starting monitor capture");
            startCapture(source);
        });
        autostartTimer->start();
    } else if (autostart == QStringLiteral("viewer")) {
        autostartViewerTimer_ = new QTimer(this);
        autostartViewerTimer_->setInterval(1000);
        auto attempts = std::make_shared<int>(0);
        QObject::connect(autostartViewerTimer_, &QTimer::timeout, this, [this, attempts] {
            if (!initialized_ || !autostartViewerTimer_) {
                if (autostartViewerTimer_) {
                    autostartViewerTimer_->stop();
                    autostartViewerTimer_->deleteLater();
                    autostartViewerTimer_ = nullptr;
                }
                return;
            }
            if (++(*attempts) > 30) {
                autostartViewerTimer_->stop();
                autostartViewerTimer_->deleteLater();
                autostartViewerTimer_ = nullptr;
                diag::log("autostart viewer: no live streams, falling back to viewer.json");
                startViewer();
                return;
            }
            if (!connected_ || !control_) {
                return;
            }
            QString room;
            QString serverUid;
            QString identity;
            QString nickname;
            int channelId = 0;
            if (!roomContext(room, serverUid, channelId, identity, nickname)) {
                return;
            }
            control_->fetchStreams(room, [this](bool ok, const QVector<ControlStream>& streams,
                                                const QString& error) {
                if (!initialized_ || !autostartViewerTimer_ || !autostartViewerTimer_->isActive()) {
                    return;
                }
                if (!ok) {
                    diag::log("autostart viewer: stream query failed: %s", error.toUtf8().constData());
                    return;
                }
                if (streams.isEmpty()) {
                    return;
                }
                autostartViewerTimer_->stop();
                autostartViewerTimer_->deleteLater();
                autostartViewerTimer_ = nullptr;
                diag::log("autostart viewer: opening stream by %s",
                          streams.first().nickname.toUtf8().constData());
                startViewerForStream(streams.first());
            });
        });
        autostartViewerTimer_->start();
    }
}

void StreamUi::shutdown() {
    if (!initialized_) {
        return;
    }
    diag::log("StreamUi shutdown");
    initialized_ = false;

    if (watchdog_) {
        watchdog_->stop();
        watchdog_ = nullptr;
    }

    if (panelTimer_) {
        panelTimer_->stop();
        panelTimer_ = nullptr;
    }
    if (membersTimer_) {
        membersTimer_->stop();
        membersTimer_ = nullptr;
    }
    if (streamsTimer_) {
        streamsTimer_->stop();
        streamsTimer_ = nullptr;
    }
    if (heartbeatTimer_) {
        heartbeatTimer_->stop();
        heartbeatTimer_ = nullptr;
    }
    if (autostartViewerTimer_) {
        autostartViewerTimer_->stop();
        autostartViewerTimer_ = nullptr;
    }

    stopCapture(true);

    if (fullscreen_ && panel_) {
        panel_->setParent(infoWrapper_);
        panel_->setWindowFlags(Qt::Widget);
        fullscreen_ = false;
    }

    // Do not delete TS3-owned Qt widgets or our timers during client shutdown:
    // TS3 and Qt tear them down themselves. Deleting them here corrupts the
    // QToolBar/layout state and is what produced the crash report.
    if (panel_) {
        panel_->removeEventFilter(this);
        panel_->hide();
        panel_ = nullptr;
    }
    if (infoFrame_) {
        infoFrame_->show();
    }
    if (watchButton_) {
        watchButton_->hide();
        watchButton_ = nullptr;
    }
    if (streamButton_) {
        streamButton_->hide();
        streamButton_ = nullptr;
    }
    streamMenu_ = nullptr;
    infoFrame_ = nullptr;
    infoWrapper_ = nullptr;
    serverTree_ = nullptr;
    if (treeViewport_) {
        treeViewport_->removeEventFilter(this);
        treeViewport_ = nullptr;
    }
    treeView_ = nullptr;
    liveDelegate_ = nullptr;
    toolBar_ = nullptr;
    mainWindow_ = nullptr;
}

void StreamUi::startFromPicker() {
    updateStreamButtonState();
    if (!connected_) {
        diag::log("StreamUi: stream requested while not connected to a server");
        return;
    }
    if (publishing_) {
        diag::log("StreamUi: stream already running, refusing a second one");
        return;
    }
    const auto source = pickCaptureSource(mainWindow_);
    if (!source) {
        return;
    }
    startCapture(*source);
}

void StreamUi::updateStreamButtonState() {
    bool connected = false;
    if (ts3_.getCurrentServerConnectionHandlerID && ts3_.getConnectionStatus) {
        const uint64 scHandler = ts3_.getCurrentServerConnectionHandlerID();
        if (scHandler != 0) {
            int status = STATUS_DISCONNECTED;
            if (ts3_.getConnectionStatus(scHandler, &status) == 0) {
                connected = status >= STATUS_CONNECTED;
            }
        }
    }

    if (streamButton_) {
        streamButton_->setEnabled(connected);
        streamButton_->setText(publishing_ ? QStringLiteral("Стрим ●") : QStringLiteral("Стрим"));
        if (publishing_) {
            streamButton_->setToolTip(QStringLiteral("Стрим идёт — открой меню, чтобы остановить"));
        } else if (connected) {
            streamButton_->setToolTip(QStringLiteral("TS3Stream — начать стрим экрана или приложения"));
        } else {
            streamButton_->setToolTip(QStringLiteral("Подключись к серверу TS3, чтобы начать стрим"));
        }
    }
    if (startAction_) {
        startAction_->setEnabled(connected && !publishing_);
    }
    if (stopAction_) {
        stopAction_->setEnabled(publishing_);
    }

    if (connected_ && !connected && captureState_) {
        diag::log("StreamUi: disconnected from server, stopping stream");
        stopCapture(false);
    }
    if (connected != connected_) {
        diag::log("StreamUi: connection state changed connected=%d", connected ? 1 : 0);
    }
    connected_ = connected;
}

void StreamUi::startViewer() {
    stopCapture();
    ensurePanel();
    if (!panel_) {
        return;
    }

    const QString path = QString::fromStdWString(diag::dataDir()) + QStringLiteral("/viewer.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        diag::log("StreamUi: viewer.json not found");
        panel_->setSubtitle(QStringLiteral("Нет viewer.json"));
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject object = document.object();
    const QString url = object.value(QStringLiteral("url")).toString();
    const QString token = object.value(QStringLiteral("token")).toString();
    if (url.isEmpty() || token.isEmpty()) {
        diag::log("StreamUi: viewer.json has no url/token");
        panel_->setSubtitle(QStringLiteral("Некорректный viewer.json"));
        return;
    }

    auto state = std::make_shared<CaptureState>();
    state->panel = panel_;
    captureState_ = state;

    auto engine = std::make_shared<media::LiveKitEngine>();
    auto renderer = std::make_shared<media::AudioRenderer>();
    if (renderer->start(48000, 2)) {
        audioRenderer_ = renderer;
    } else {
        diag::log("StreamUi: viewer audio renderer failed to start");
        renderer.reset();
    }

    engine->setVideoFrameCallback([state](int width, int height, const uint8_t* rgba, int stride, int64_t) {
        if (!state->alive.load() || width <= 0 || height <= 0 || !rgba) {
            return;
        }
        QImage image(width, height, QImage::Format_RGBA8888);
        for (int y = 0; y < height; ++y) {
            std::memcpy(image.scanLine(y), rgba + static_cast<size_t>(y) * stride,
                        static_cast<size_t>(width) * 4);
        }
        std::lock_guard<std::mutex> lock(state->frameMutex);
        state->latestFrame = image;
        state->latestWidth = width;
        state->latestHeight = height;
    });
    engine->setAudioFrameCallback([renderer](const int16_t* samples, int frames, int channels, int sampleRate) {
        static std::atomic<int> counter{0};
        if ((counter.fetch_add(1) % 300) == 0) {
            diag::log("viewer audio frame: frames=%d channels=%d rate=%d", frames, channels, sampleRate);
        }
        if (renderer) {
            renderer->push(samples, frames, channels);
        }
    });

    std::string error;
    if (!engine->connect(url.toStdString(), token.toStdString(), true, error)) {
        diag::log("StreamUi: viewer connect failed: %s", error.c_str());
        panel_->setSubtitle(QStringLiteral("Ошибка подключения: ") + QString::fromStdString(error));
        state->alive.store(false);
        captureState_.reset();
        return;
    }

    livekit_ = engine;
    startPanelTimer();
    panel_->setTitle(QStringLiteral("Тестовый стрим"));
    panel_->setSubtitle(QStringLiteral("LiveKit viewer"));
    panel_->setLive(true);
    panel_->setAudioStatus(QStringLiteral("подключён"));
    diag::log("StreamUi: viewer connected");
}

void StreamUi::startViewerForStream(const ControlStream& stream) {
    if (stream.nickname.isEmpty() || stream.room.isEmpty() || stream.identity.isEmpty()) {
        return;
    }
    startViewerForRoom(stream.room, stream.identity, stream.nickname);
}

void StreamUi::startViewerForRoom(const QString& room, const QString& targetIdentity, const QString& nickname) {
    if (!control_) {
        diag::log("StreamUi: viewer requested without control client");
        return;
    }

    stopCapture();
    ensurePanel();
    if (!panel_) {
        return;
    }

    QString myRoom;
    QString serverUid;
    QString myIdentity;
    QString myNickname;
    int channelId = 0;
    if (!roomContext(myRoom, serverUid, channelId, myIdentity, myNickname)) {
        myIdentity = QStringLiteral("viewer-%1").arg(GetCurrentProcessId());
        myNickname = QStringLiteral("Viewer");
    }
    const QString viewerIdentity = QStringLiteral("%1-viewer-%2")
                                       .arg(myIdentity)
                                       .arg(QDateTime::currentMSecsSinceEpoch());

    const int generation = ++captureGeneration_;
    panel_->setTitle(QStringLiteral("Стрим: %1").arg(nickname));
    panel_->setSubtitle(QStringLiteral("Запрос токена…"));
    panel_->setLive(false);
    panel_->setStats(0, 0, 0);
    panel_->setAudioStatus(QStringLiteral("подключение"));

    diag::log("StreamUi: requesting viewer token room=%s target=%s",
              room.toUtf8().constData(), targetIdentity.toUtf8().constData());
    control_->fetchToken(room, viewerIdentity, myNickname, false,
                         [this, generation, room, targetIdentity, nickname](
                             bool ok, const LiveKitToken& token, const QString& error) {
        if (generation != captureGeneration_ || !initialized_) {
            return;
        }
        if (!ok) {
            diag::log("StreamUi: viewer token failed: %s", error.toUtf8().constData());
            if (panel_) {
                panel_->setSubtitle(QStringLiteral("Control API: ") + error);
            }
            return;
        }
        if (!panel_) {
            return;
        }

        auto state = std::make_shared<CaptureState>();
        state->panel = panel_;
        state->title = QStringLiteral("Стрим: %1").arg(nickname);
        captureState_ = state;

        auto engine = std::make_shared<media::LiveKitEngine>();
        auto renderer = std::make_shared<media::AudioRenderer>();
        if (renderer->start(48000, 2)) {
            audioRenderer_ = renderer;
        } else {
            diag::log("StreamUi: viewer audio renderer failed to start");
            renderer.reset();
        }

        engine->setVideoFrameCallback([state](int width, int height, const uint8_t* rgba, int stride, int64_t) {
            if (!state->alive.load() || width <= 0 || height <= 0 || !rgba) {
                return;
            }
            QImage image(width, height, QImage::Format_RGBA8888);
            for (int y = 0; y < height; ++y) {
                std::memcpy(image.scanLine(y), rgba + static_cast<size_t>(y) * stride,
                            static_cast<size_t>(width) * 4);
            }
            std::lock_guard<std::mutex> lock(state->frameMutex);
            state->latestFrame = image;
            state->latestWidth = width;
            state->latestHeight = height;
        });
        engine->setAudioFrameCallback([renderer](const int16_t* samples, int frames, int channels, int sampleRate) {
            if (renderer) {
                renderer->push(samples, frames, channels);
            }
        });

        std::string connectError;
        if (!engine->connect(token.url.toStdString(), token.token.toStdString(), true, connectError,
                             targetIdentity.toStdString())) {
            diag::log("StreamUi: viewer connect failed: %s", connectError.c_str());
            panel_->setSubtitle(QStringLiteral("Ошибка подключения: ") + QString::fromStdString(connectError));
            state->alive.store(false);
            captureState_.reset();
            return;
        }

        livekit_ = engine;
        startPanelTimer();
        panel_->setTitle(QStringLiteral("Стрим: %1").arg(nickname));
        panel_->setSubtitle(QStringLiteral("канал %1 · LiveKit").arg(room));
        panel_->setLive(true);
        panel_->setAudioStatus(QStringLiteral("подключён"));
        diag::log("StreamUi: viewer connected to %s, target=%s",
                  room.toUtf8().constData(), targetIdentity.toUtf8().constData());
    });
}

void StreamUi::startCapture(const CaptureSource& source) {
    if (!connected_) {
        diag::log("StreamUi: capture requested while not connected to a server, ignoring");
        return;
    }
    if (publishing_) {
        diag::log("StreamUi: stream already running, refusing a second one");
        return;
    }
    if (!control_) {
        diag::log("StreamUi: control client unavailable");
        return;
    }

    QString room;
    QString serverUid;
    QString identity;
    QString nickname;
    int channelId = 0;
    if (!roomContext(room, serverUid, channelId, identity, nickname)) {
        diag::log("StreamUi: cannot resolve TS3 channel room");
        ensurePanel();
        if (panel_) {
            panel_->setSubtitle(QStringLiteral("Не удалось определить канал TS3"));
        }
        return;
    }

    stopCapture();
    ensurePanel();
    if (!panel_) {
        diag::log("StreamUi: no panel, cannot start capture");
        return;
    }

    const int generation = ++captureGeneration_;
    panel_->setTitle(source.title);
    panel_->setSubtitle(QStringLiteral("Получение токена…"));
    panel_->setLive(false);
    panel_->setStats(0, 0, 0);
    panel_->setAudioStatus(QStringLiteral("подключение"));

    diag::log("StreamUi: requesting publish token room=%s identity=%s",
              room.toUtf8().constData(), identity.toUtf8().constData());
    control_->fetchToken(room, identity, nickname, true,
                         [this, source, generation, room, identity, nickname, channelId, serverUid](
                             bool ok, const LiveKitToken& token, const QString& error) {
        if (generation != captureGeneration_ || !initialized_ || !connected_) {
            return;
        }
        if (!ok) {
            diag::log("StreamUi: publish token failed: %s", error.toUtf8().constData());
            if (panel_) {
                panel_->setSubtitle(QStringLiteral("Control API: ") + error);
                panel_->setAudioStatus(QStringLiteral("нет токена"));
            }
            return;
        }
        beginCapture(source, token, room, identity, nickname, channelId, serverUid);
    });
}

void StreamUi::beginCapture(const CaptureSource& source, const LiveKitToken& token,
                            const QString& room, const QString& identity, const QString& nickname,
                            int channelId, const QString& serverUid) {
    stopCapture();
    ensurePanel();
    if (!panel_) {
        diag::log("StreamUi: no panel, cannot start capture");
        return;
    }

    auto state = std::make_shared<CaptureState>();
    state->panel = panel_;
    state->title = source.title;
    state->fps = source.fps > 0 ? source.fps : 30;
    state->autoFps = source.autoFps;
    state->started = std::chrono::steady_clock::now();
    captureState_ = state;
    publishing_ = true;
    diag::log("StreamUi: capture fps=%d auto=%d", state->fps, state->autoFps ? 1 : 0);

    panel_->setTitle(source.title);
    panel_->setSubtitle(sourceSubtitle(source));
    panel_->setLive(false);
    panel_->setStats(0, 0, 0);
    panel_->setAudioStatus(source.audioType == CaptureSource::AudioType::None
                               ? QStringLiteral("выключен")
                               : QStringLiteral("запуск…"));

    startPanelTimer();

    std::shared_ptr<media::LiveKitEngine> livekitEngine;
    if (token.url.isEmpty() || token.token.isEmpty()) {
        diag::log("StreamUi: control API returned an empty token");
        stopCapture();
        ensurePanel();
        if (panel_) {
            panel_->setSubtitle(sourceSubtitle(source) + QStringLiteral(" · нет токена"));
        }
        return;
    } else {
        livekitEngine = std::make_shared<media::LiveKitEngine>();
        std::string error;
        if (livekitEngine->connect(token.url.toStdString(), token.token.toStdString(), false, error)) {
            livekit_ = livekitEngine;
            state->livekit = livekitEngine;
            panel_->setSubtitle(sourceSubtitle(source) + QStringLiteral(" · LiveKit"));
            diag::log("StreamUi: LiveKit connected");
        } else {
            diag::log("StreamUi: LiveKit connect failed: %s", error.c_str());
            stopCapture();
            ensurePanel();
            if (panel_) {
                panel_->setSubtitle(sourceSubtitle(source) + QStringLiteral(" · LiveKit: ошибка"));
                panel_->setAudioStatus(QStringLiteral("нет соединения"));
            }
            return;
        }
    }

    if (source.audioType != CaptureSource::AudioType::None &&
        qEnvironmentVariableIsEmpty("TS3STREAM_DISABLE_AUDIO")) {
        auto audioState = std::make_shared<AudioState>();
        audioState->panel = panel_;
        audioState->sourceName = source.audioType == CaptureSource::AudioType::Process
                                     ? QStringLiteral("приложение")
                                     : QStringLiteral("система (без TeamSpeak)");
        audioState->livekit = livekitEngine;
        audioState_ = audioState;

        audioCapture_ = std::make_unique<media::RemoteAudioCapture>();
        auto audioCallback = [audioState](const media::CapturedAudioFrame& frame) {
            if (!audioState->alive.load()) {
                return;
            }

            if (audioState->livekit && audioState->livekit->isConnected()) {
                if (!audioState->livekitAudioReady.load()) {
                    std::string error;
                    if (audioState->livekit->ensureAudio(error)) {
                        audioState->livekitAudioReady.store(true);
                    } else if (!error.empty()) {
                        diag::log("LiveKit audio setup failed: %s", error.c_str());
                    }
                }
                if (audioState->livekitAudioReady.load()) {
                    audioState->livekit->pushAudio(frame);
                }
            }

            if (!audioState->panel) {
                return;
            }
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
            const auto previous = audioState->lastUpdateMs.exchange(nowMs);
            if (previous != 0 && nowMs - previous < 150) {
                return;
            }

            const double amplitude = std::max(1.0, static_cast<double>(frame.peak)) / 32768.0;
            const double db = 20.0 * std::log10(amplitude);
            const int percent = static_cast<int>(std::clamp((db + 60.0) / 60.0 * 100.0, 0.0, 100.0));
            if ((audioState->logCounter.fetch_add(1) % 60) == 0) {
                diag::log("audio peak=%d percent=%d frames=%d", frame.peak, percent, frame.frames);
            }
            QMetaObject::invokeMethod(qApp, [audioState, percent] {
                if (audioState->alive.load() && audioState->panel) {
                    audioState->panel->setAudioStatus(
                        QStringLiteral("%1 · %2%").arg(audioState->sourceName).arg(percent));
                }
            }, Qt::QueuedConnection);
        };

        bool audioOk = false;
        if (source.audioType == CaptureSource::AudioType::Process &&
            source.processId != GetCurrentProcessId()) {
            audioOk = audioCapture_->startApp(source.processId, audioCallback);
        } else if (source.audioType == CaptureSource::AudioType::Device) {
            // The helper process captures system audio in process-loopback
            // exclude mode, excluding the TS3 process tree. Device loopback is
            // never used for streaming because it would capture TS3 itself.
            audioOk = audioCapture_->startSystemExcludingTs3(GetCurrentProcessId(), audioCallback);
        }

        if (!audioOk) {
            diag::log("StreamUi: audio capture failed: %s", audioCapture_->lastError().c_str());
            panel_->setAudioStatus(QStringLiteral("недоступно (TS3 исключён)"));
            audioCapture_.reset();
            audioState_.reset();
        } else {
            diag::log("StreamUi: audio capture started (TS3 excluded)");
            QTimer::singleShot(1500, this, [this] {
                if (audioCapture_ && !audioCapture_->isRunning() && panel_) {
                    panel_->setAudioStatus(QStringLiteral("недоступно (TS3 не захватывается)"));
                    diag::log("StreamUi: audio helper stopped: %s", audioCapture_->lastError().c_str());
                }
            });
        }
    }

    if (!qEnvironmentVariableIsEmpty("TS3STREAM_DISABLE_VIDEO")) {
        diag::log("StreamUi: video capture disabled by TS3STREAM_DISABLE_VIDEO");
        stopCapture();
        ensurePanel();
        if (panel_) {
            panel_->setAudioStatus(QStringLiteral("видео отключено для теста"));
        }
        return;
    }

    capture_ = std::make_unique<media::WgcCapture>();
    capture_->setTargetFps(state->fps);
    frameProcessor_ = std::make_shared<FrameProcessor>([this, state](const media::CapturedVideoFrame& frame) {
        processVideoFrame(state, frame);
    });
    frameProcessor_->start();

    auto callback = [state, processor = frameProcessor_](media::CapturedVideoFrame frame) {
        if (!state->alive.load()) {
            return;
        }

        // WGC already limits the frame rate; keep this callback cheap and let
        // the FrameProcessor thread do the QImage conversion and encoding.
        processor->push(std::move(frame));
    };

    bool ok = false;
    if (source.type == CaptureSource::Type::Monitor) {
        ok = capture_->startMonitor(source.monitor, callback);
    } else if (source.type == CaptureSource::Type::Window) {
        ok = capture_->startWindow(source.window, callback);
    }

    if (!ok) {
        const std::string captureError = capture_->lastError();
        diag::log("StreamUi: capture start failed: %s", captureError.c_str());
        stopCapture();
        ensurePanel();
        if (panel_) {
            panel_->setSubtitle(QStringLiteral("Ошибка захвата: ") + QString::fromStdString(captureError));
            panel_->setLive(false);
        }
        return;
    }

    panel_->setLive(true);
    if (control_) {
        activeRoom_ = room;
        activeIdentity_ = identity;
        activeNickname_ = nickname;
        activeServerUid_ = serverUid;
        activeChannelId_ = channelId;
        control_->announceStart(room, identity, nickname, channelId, serverUid);
        if (!heartbeatTimer_) {
            heartbeatTimer_ = new QTimer(this);
            heartbeatTimer_->setInterval(5000);
            QObject::connect(heartbeatTimer_, &QTimer::timeout, this, [this] {
                if (control_ && !activeRoom_.isEmpty() && !activeIdentity_.isEmpty()) {
                    // Start is idempotent: this also recovers the registry after
                    // a control-service restart or a missed 15 s TTL.
                    control_->announceStart(activeRoom_, activeIdentity_, activeNickname_,
                                            activeChannelId_, activeServerUid_);
                }
            });
        }
        heartbeatTimer_->start();
    }
    diag::log("StreamUi: capture started for \"%s\"", source.title.toUtf8().constData());
}

void StreamUi::startPanelTimer() {
    if (!panelTimer_) {
        panelTimer_ = new QTimer(this);
        panelTimer_->setInterval(33);
        QObject::connect(panelTimer_, &QTimer::timeout, this, [this] {
            auto state = captureState_;
            if (!state || !state->alive.load() || !panel_) {
                return;
            }
            QImage image;
            int fps = 0;
            int width = 0;
            int height = 0;
            {
                std::lock_guard<std::mutex> lock(state->frameMutex);
                image = state->latestFrame;
                fps = state->latestFps;
                width = state->latestWidth;
                height = state->latestHeight;
            }
            if (!image.isNull()) {
                panel_->setFrame(image);
                if (fps > 0) {
                    panel_->setStats(fps, width, height);
                }
            }
        });
    }
    panelTimer_->start();
}

void StreamUi::processVideoFrame(const std::shared_ptr<CaptureState>& state,
                                 const media::CapturedVideoFrame& frame) {
    if (!state->alive.load()) {
        return;
    }

    const int frameWidth = frame.width;
    const int frameHeight = frame.height;
    if (frameWidth <= 0 || frameHeight <= 0) {
        return;
    }

    QImage image(frameWidth, frameHeight, QImage::Format_ARGB32);
    if (image.isNull()) {
        return;
    }

    const int rowBytes = frameWidth * 4;
    for (int y = 0; y < frameHeight; ++y) {
        const uint8_t* src = frame.bgra.data() + static_cast<size_t>(y) * frame.stride;
        std::memcpy(image.scanLine(y), src, rowBytes);
    }

    // Screen text is the hard part: 720p from a 1440p desktop plus a fast
    // downscale made glyphs unreadable. Encode 1080p and use a smooth filter.
    constexpr int kMaxWidth = 1920;
    constexpr int kMaxHeight = 1080;
    if (image.width() > kMaxWidth || image.height() > kMaxHeight) {
        image = image.scaled(kMaxWidth, kMaxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    media::CapturedVideoFrame scaledFrame;
    scaledFrame.width = image.width();
    scaledFrame.height = image.height();
    scaledFrame.stride = image.bytesPerLine();
    scaledFrame.timestampUs = frame.timestampUs;
    scaledFrame.bgra.assign(image.bits(), image.bits() + image.sizeInBytes());

    if (state->livekit && state->livekit->isConnected()) {
        if (!state->livekitVideoReady.load()) {
            std::string error;
            if (state->livekit->ensureVideo(scaledFrame.width, scaledFrame.height, state->fps, error)) {
                state->livekitVideoReady.store(true);
            } else if (!error.empty()) {
                diag::log("LiveKit video setup failed: %s", error.c_str());
            }
        }
        if (state->livekitVideoReady.load()) {
            state->livekit->pushVideo(std::move(scaledFrame));
        }
    }

    const int frames = state->frames.fetch_add(1) + 1;
    if (frames == 1) {
        state->started = std::chrono::steady_clock::now();
    }

    int fps = 0;
    if ((frames % 30) == 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - state->started)
                                 .count();
        if (elapsed > 0) {
            fps = static_cast<int>((frames * 1000) / elapsed);
            diag::log("video fps=%d frames=%d size=%dx%d", fps, frames, frameWidth, frameHeight);
        }
    }

    {
        std::lock_guard<std::mutex> lock(state->frameMutex);
        state->latestFrame = image;
        state->latestWidth = frameWidth;
        state->latestHeight = frameHeight;
        if (fps > 0) {
            state->latestFps = fps;
        }
    }
}

void StreamUi::stopCapture(bool forShutdown) {
    ++captureGeneration_;
    publishing_ = false;
    if (heartbeatTimer_) {
        heartbeatTimer_->stop();
    }
    if (control_ && !activeRoom_.isEmpty() && !activeIdentity_.isEmpty()) {
        control_->announceStop(activeRoom_, activeIdentity_);
    }
    activeRoom_.clear();
    activeIdentity_.clear();
    activeNickname_.clear();
    activeServerUid_.clear();
    activeChannelId_ = 0;

    if (panelTimer_) {
        panelTimer_->stop();
    }
    if (captureState_) {
        captureState_->alive.store(false);
    }
    if (audioState_) {
        audioState_->alive.store(false);
    }

    // Stop producers before disconnecting LiveKit so no callback can race
    // with teardown of the room or the FFI SDK.
    if (capture_) {
        capture_->stop();
        retiredCaptures_.push_back(std::move(capture_));
    }
    if (audioCapture_) {
        audioCapture_->stop();
        retiredAudioCaptures_.push_back(std::move(audioCapture_));
    }
    if (frameProcessor_) {
        frameProcessor_->stop();
        retiredFrameProcessors_.push_back(std::move(frameProcessor_));
    }
    if (audioRenderer_) {
        audioRenderer_->stop();
        retiredAudioRenderers_.push_back(std::move(audioRenderer_));
    }
    if (livekit_) {
        if (forShutdown) {
            livekit_->disconnectForShutdown();
        } else {
            livekit_->disconnect();
        }
        livekit_.reset();
    }
    audioState_.reset();
    captureState_.reset();

    if (panel_) {
        panel_->setLive(false);
        panel_->setStats(0, 0, 0);
    }
}

void StreamUi::ensurePanel() {
    if (!infoFrame_) {
        infoFrame_ = findVisibleInfoFrame(mainWindow_);
        if (infoFrame_) {
            infoWrapper_ = infoFrame_->parentWidget();
        }
    }
    if (!infoFrame_ || !infoWrapper_) {
        diag::log("StreamUi: InfoFrame not found");
        return;
    }

    if (!panel_) {
        panel_ = new StreamPanel(infoWrapper_);
        panel_->setOnClose([this] {
            stopCapture();
            restoreInfoPanel();
        });
        panel_->setOnFullscreen([this] { toggleFullscreen(); });
        panel_->installEventFilter(this);
    }

    infoFrame_->hide();
    panel_->setParent(infoWrapper_);
    panel_->setWindowFlags(Qt::Widget);
    refreshPanelGeometry();
    panel_->show();
    panel_->raise();
    if (watchButton_) {
        watchButton_->hide();
    }
}

void StreamUi::restoreInfoPanel() {
    if (panel_) {
        panel_->hide();
        panel_->deleteLater();
        panel_ = nullptr;
    }
    if (infoFrame_) {
        infoFrame_->show();
    }
    updateWatchButton();
}

void StreamUi::refreshPanelGeometry() {
    if (!panel_ || !infoWrapper_ || fullscreen_) {
        return;
    }
    if (panel_->parentWidget() != infoWrapper_) {
        panel_->setParent(infoWrapper_);
        panel_->setWindowFlags(Qt::Widget);
        panel_->show();
    }
    panel_->setGeometry(infoWrapper_->rect());
    if (panel_->size() != lastPanelSize_) {
        lastPanelSize_ = panel_->size();
        diag::log("StreamUi: panel resized to %dx%d (wrapper %dx%d)",
                  lastPanelSize_.width(), lastPanelSize_.height(),
                  infoWrapper_->width(), infoWrapper_->height());
    }
    panel_->raise();
}

void StreamUi::toggleFullscreen() {
    if (!panel_) {
        return;
    }

    if (!fullscreen_) {
        // showFullScreen() on a freshly reparented widget otherwise falls back
        // to the primary screen. Use the screen under the TS3 window's actual
        // global position; panel_->screen() can be stale after earlier moves.
        QScreen* targetScreen = nullptr;
        if (mainWindow_) {
            targetScreen = QGuiApplication::screenAt(mainWindow_->frameGeometry().center());
            if (!targetScreen) {
                targetScreen = mainWindow_->screen();
            }
        }
        if (!targetScreen) {
            targetScreen = panel_->screen();
        }
        if (!targetScreen) {
            targetScreen = QGuiApplication::primaryScreen();
        }
        const QRect targetGeometry = targetScreen ? targetScreen->geometry() : QRect();
        diag::log("StreamUi: fullscreen target main=%s panel=%s screen=%s geo=%d,%d %dx%d",
                  mainWindow_ && mainWindow_->screen()
                      ? mainWindow_->screen()->name().toUtf8().constData()
                      : "null",
                  panel_->screen() ? panel_->screen()->name().toUtf8().constData() : "null",
                  targetScreen ? targetScreen->name().toUtf8().constData() : "unknown",
                  targetGeometry.x(), targetGeometry.y(),
                  targetGeometry.width(), targetGeometry.height());

        // Set the flag before reparenting/showing: the 100 ms geometry watchdog
        // must not pull the panel back into the InfoFrame during the transition.
        fullscreen_ = true;
        panel_->setParent(nullptr);
        panel_->setWindowFlags(Qt::Window);
        panel_->winId();
        if (targetScreen) {
            if (QWindow* window = panel_->windowHandle()) {
                window->setScreen(targetScreen);
            }
            panel_->setGeometry(targetGeometry);
        }
        panel_->showFullScreen();
        panel_->raise();
        panel_->setFullscreenMode(true);
        diag::log("StreamUi: fullscreen on screen=%s",
                  targetScreen ? targetScreen->name().toUtf8().constData() : "unknown");
    } else {
        panel_->setParent(infoWrapper_);
        panel_->setWindowFlags(Qt::Widget);
        panel_->show();
        panel_->setFullscreenMode(false);
        fullscreen_ = false;
        refreshPanelGeometry();
        diag::log("StreamUi: fullscreen off");
    }
}

bool StreamUi::eventFilter(QObject* watched, QEvent* event) {
    if (treeView_ && treeViewport_ && watched == treeViewport_ &&
        event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const QModelIndex rowIndex = treeView_->indexAt(QPoint(5, mouse->pos().y()));
            if (rowIndex.isValid()) {
                const QString nickname = rowIndex.data(Qt::DisplayRole).toString().trimmed();
                const auto it = liveStreams_.constFind(nickname);
                const QRect rowRect = treeView_->visualRect(rowIndex);
                const bool inBadgeZone = mouse->pos().x() >= treeViewport_->width() - 56 &&
                                         mouse->pos().y() >= rowRect.top() - 2 &&
                                         mouse->pos().y() <= rowRect.bottom() + 2;
                if (it != liveStreams_.constEnd() && inBadgeZone) {
                    diag::log("StreamUi: LIVE badge clicked: %s", nickname.toUtf8().constData());
                    QMenu menu(treeView_);
                    QAction* watch = menu.addAction(
                        QStringLiteral("▶  Смотреть стрим — %1").arg(nickname));
                    if (menu.exec(mouse->globalPos()) == watch) {
                        startViewerForRoom(it.value().room, it.value().identity, it.value().nickname);
                    }
                    return true;
                }
            }
        }
    }

    if (watched == panel_ && fullscreen_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            toggleFullscreen();
            return true;
        }
    }

    return QObject::eventFilter(watched, event);
}

} // namespace ts3stream::ui
