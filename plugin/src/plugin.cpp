#include "diag/ui_probe.h"
#include "diag/widget_dump.h"
#include "log.h"
#include "ui/stream_ui.h"

#include "ts3_functions.h"
#include "ts3plugin.h"

#include <windows.h>

#include <QApplication>
#include <QDir>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {

struct TS3Functions g_ts3Functions = {};
char* g_pluginId = nullptr;
std::atomic<bool> g_running{false};
std::thread g_bootstrapThread;
QPointer<QTimer> g_diagTimer;

std::string configPath() {
    char path[4096] = {};
    if (g_ts3Functions.getConfigPath) {
        g_ts3Functions.getConfigPath(path, sizeof(path));
    }
    if (path[0] == '\0') {
        return ts3stream::diag::toUtf8(ts3stream::diag::dataDir());
    }
    return path;
}

QString dumpFilePath() {
    const QString dir = QString::fromStdString(configPath()) + "/plugins/ts3stream";
    QDir().mkpath(dir);
    return dir + "/widgets.txt";
}

void runQtDiagnostics() {
    ts3stream::diag::log("runQtDiagnostics on Qt thread tid=%p",
                         reinterpret_cast<void*>(QThread::currentThreadId()));

    const QString path = dumpFilePath();
    ts3stream::diag::dumpWidgetTree(path);
    ts3stream::ui::StreamUi::instance().setTs3Functions(g_ts3Functions);
    if (qEnvironmentVariableIsEmpty("TS3STREAM_DISABLE_UI")) {
        ts3stream::ui::StreamUi::instance().initialize();
    } else {
        ts3stream::diag::log("StreamUi disabled by TS3STREAM_DISABLE_UI");
    }
    ts3stream::diag::dumpWidgetTree(path + ".after-ui.txt");
}

void retryUiInitialization() {
    if (!qApp || ts3stream::ui::StreamUi::instance().isInitialized()) {
        return;
    }
    ts3stream::diag::log("StreamUi: retrying UI initialization");
    ts3stream::ui::StreamUi::instance().initialize();
    if (!ts3stream::ui::StreamUi::instance().isInitialized() && qApp) {
        QTimer::singleShot(500, qApp, [] { retryUiInitialization(); });
    }
}

void stopDiagnosticsTimer() {
    if (g_diagTimer) {
        g_diagTimer->stop();
        // Do not delete: Qt owns the timer and is tearing down the application.
        g_diagTimer = nullptr;
        ts3stream::diag::log("diagnostics timer stopped");
    }
}

void startDiagnosticsTimer() {
    if (g_diagTimer) {
        return;
    }

    auto* timer = new QTimer(qApp);
    timer->setSingleShot(true);
    timer->setInterval(3000);
    QObject::connect(timer, &QTimer::timeout, qApp, [timer] {
        if (!g_running.load()) {
            return;
        }
        runQtDiagnostics();
        if (!ts3stream::ui::StreamUi::instance().isInitialized()) {
            QTimer::singleShot(500, qApp, [] { retryUiInitialization(); });
        }
        g_running.store(false);
        if (g_diagTimer == timer) {
            g_diagTimer = nullptr;
        }
        timer->deleteLater();
    });
    g_diagTimer = timer;
    timer->start();
    ts3stream::diag::log("diagnostics timer started (3 s)");
}

void ensureRuntimeDlls() {
    const std::filesystem::path dataDir(ts3stream::diag::dataDir());
    const std::filesystem::path moduleDir(ts3stream::diag::moduleDir());
    const std::filesystem::path packagedDir = moduleDir / L"ts3stream";

    for (const wchar_t* name : {L"livekit.dll", L"livekit_ffi.dll"}) {
        const std::filesystem::path destination = dataDir / name;
        std::error_code ec;
        std::filesystem::path source = packagedDir / name;
        if (!std::filesystem::exists(source, ec)) {
            source = moduleDir / name;
        }
        if (!std::filesystem::exists(source, ec)) {
            ts3stream::diag::log("runtime DLL not found next to plugin: %ls", name);
            continue;
        }

        bool needsCopy = !std::filesystem::exists(destination, ec);
        if (!needsCopy) {
            const auto sourceTime = std::filesystem::last_write_time(source, ec);
            const auto destinationTime = std::filesystem::last_write_time(destination, ec);
            needsCopy = !ec && sourceTime > destinationTime;
            if (!needsCopy) {
                needsCopy = std::filesystem::file_size(source, ec) !=
                            std::filesystem::file_size(destination, ec);
            }
        }
        if (!needsCopy) {
            continue;
        }

        std::filesystem::create_directories(dataDir, ec);
        std::filesystem::copy_file(source, destination,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            ts3stream::diag::log("failed to stage runtime DLL %ls: %s", name, ec.message().c_str());
        } else {
            ts3stream::diag::log("runtime DLL staged: %ls", name);
        }
    }
}

void bootstrapThreadMain() {
    ts3stream::diag::log("bootstrap thread started");

    QApplication* app = nullptr;
    for (int i = 0; i < 300 && g_running.load(); ++i) {
        app = qobject_cast<QApplication*>(QApplication::instance());
        if (app) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!app) {
        ts3stream::diag::log("bootstrap: QApplication not found");
        return;
    }

    ts3stream::diag::log("bootstrap: QApplication found, scheduling diagnostics");
    QMetaObject::invokeMethod(app, [] {
        startDiagnosticsTimer();
    }, Qt::QueuedConnection);
}

} // namespace

extern "C" {

const char* ts3plugin_name() {
    return "TS3Stream";
}

const char* ts3plugin_version() {
    return "0.3.1";
}

int ts3plugin_apiVersion() {
    return 26;
}

const char* ts3plugin_author() {
    return "TS3Stream";
}

const char* ts3plugin_description() {
    return "Screen and application streaming for TeamSpeak 3 with LiveKit.";
}

void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    g_ts3Functions = funcs;
}

int ts3plugin_init() {
    ts3stream::diag::initLog();
    ensureRuntimeDlls();

    // Pin this DLL for the lifetime of the TS3 process. The LiveKit FFI and
    // Qt shutdown paths may still reference plugin code after FreeLibrary();
    // keeping the module mapped prevents an access violation on client exit.
    HMODULE selfModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&ts3plugin_init), &selfModule)) {
        ts3stream::diag::log("plugin DLL pinned");
    }

    ts3stream::diag::log("ts3plugin_init: TS3Stream 0.3.1");
    ts3stream::diag::log("init thread tid=%p", reinterpret_cast<void*>(QThread::currentThreadId()));

    g_running.store(true);
    g_bootstrapThread = std::thread(bootstrapThreadMain);
    return 0;
}

void ts3plugin_shutdown() {
    ts3stream::diag::log("ts3plugin_shutdown");
    g_running.store(false);

    if (g_bootstrapThread.joinable()) {
        g_bootstrapThread.join();
    }

    QApplication* app = qobject_cast<QApplication*>(QApplication::instance());
    if (app) {
        auto cleanup = [] {
            stopDiagnosticsTimer();
            ts3stream::ui::StreamUi::instance().shutdown();
            ts3stream::diag::removeProbeUi();
        };

        if (QThread::currentThread() == app->thread()) {
            cleanup();
        } else if (app->thread()->isRunning()) {
            QMetaObject::invokeMethod(app, cleanup, Qt::BlockingQueuedConnection);
        }
    }

    if (g_pluginId) {
        free(g_pluginId);
        g_pluginId = nullptr;
    }
}

void ts3plugin_registerPluginID(const char* id) {
    if (!id) {
        return;
    }
    const size_t length = strlen(id) + 1;
    g_pluginId = static_cast<char*>(malloc(length));
    if (g_pluginId) {
        strcpy_s(g_pluginId, length, id);
    }
    ts3stream::diag::log("registerPluginID: %s", id);
}

int ts3plugin_offersConfigure() {
    return PLUGIN_OFFERS_NO_CONFIGURE;
}

int ts3plugin_requestAutoload() {
    return 1;
}

void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status,
                                        int isReceivedWhisper, anyID clientID) {
    (void)isReceivedWhisper;
    ts3stream::ui::StreamUi::instance().onTalkStatusChanged(
        serverConnectionHandlerID, clientID, status != STATUS_NOT_TALKING);
}

void ts3plugin_freeMemory(void* data) {
    free(data);
}

void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon) {
    if (menuIcon) {
        *menuIcon = nullptr;
    }
    if (!menuItems) {
        return;
    }

    auto** items = static_cast<PluginMenuItem**>(malloc(sizeof(PluginMenuItem*) * 2));
    auto* item = static_cast<PluginMenuItem*>(malloc(sizeof(PluginMenuItem)));
    if (!items || !item) {
        free(items);
        free(item);
        *menuItems = nullptr;
        return;
    }

    memset(item, 0, sizeof(*item));
    item->type = PLUGIN_MENU_TYPE_CLIENT;
    item->id = 1;
    strcpy_s(item->text, PLUGIN_MENU_BUFSZ, "Смотреть стрим (TS3Stream)");

    items[0] = item;
    items[1] = nullptr;
    *menuItems = items;
    ts3stream::diag::log("client context menu item registered");
}

void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type,
                               int menuItemID, uint64 selectedItemID) {
    if (type != PLUGIN_MENU_TYPE_CLIENT || menuItemID != 1) {
        return;
    }
    const anyID clientID = static_cast<anyID>(selectedItemID);
    ts3stream::diag::log("context menu stream requested: client=%u", static_cast<unsigned>(clientID));

    QApplication* app = qobject_cast<QApplication*>(QApplication::instance());
    if (app) {
        QMetaObject::invokeMethod(app, [serverConnectionHandlerID, clientID] {
            ts3stream::ui::StreamUi::instance().openStreamForClient(serverConnectionHandlerID, clientID);
        }, Qt::QueuedConnection);
    }
}

} // extern "C"
