#include "ui/live_stream_menu.h"
#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QTimer>
#include <QWidget>
#include <cstdio>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QWidget host;
    host.show();
    QHash<QString, ts3stream::ui::LiveStreamInfo> registry;
    const ts3stream::ui::LiveStreamInfo original{QStringLiteral("room-a"), QStringLiteral("uid+/="),
                                               QStringLiteral("Alice"), 1};
    registry.insert(original.nickname, original);
    bool chosen = false;
    auto* menu = ts3stream::ui::popupLiveStreamMenu(&host, registry[original.nickname],
        host.mapToGlobal(QPoint(0, 0)), [&](ts3stream::ui::LiveStreamInfo selection) {
            chosen = selection.room == original.room && selection.identity == original.identity &&
                     selection.nickname == original.nickname;
        });
    QTimer::singleShot(0, &app, [&] {
        // The live registry refreshes repeatedly while the menu stays open.
        registry.clear();
        for (int i = 0; i < 10000; ++i) {
            registry.insert(QString::number(i), {QString::number(i), QStringLiteral("other"), QString::number(i), 2});
        }
        menu->actions().first()->trigger();
        menu->close();
        std::printf("menu selection after registry replacement: %s\n", chosen ? "PASS" : "FAIL");
        app.exit(chosen ? 0 : 1);
    });
    return app.exec();
}
