#include "widget_dump.h"

#include "../log.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QWidget>

#include <algorithm>
#include <functional>

namespace ts3stream::diag {
namespace {

QString clean(QString value) {
    value.replace('\r', ' ');
    value.replace('\n', ' ');
    value.replace('\t', ' ');
    if (value.size() > 300) {
        value.truncate(300);
        value += "...";
    }
    return value;
}

QString safeText(const QWidget* widget) {
    if (!widget) {
        return {};
    }
    if (const auto* button = qobject_cast<const QAbstractButton*>(widget)) {
        return clean(button->text());
    }
    if (const auto* label = qobject_cast<const QLabel*>(widget)) {
        return clean(label->text());
    }
    if (const auto* lineEdit = qobject_cast<const QLineEdit*>(widget)) {
        return clean(lineEdit->text());
    }
    if (const auto* plain = qobject_cast<const QPlainTextEdit*>(widget)) {
        return clean(plain->toPlainText().left(120));
    }
    if (const auto* textEdit = qobject_cast<const QTextEdit*>(widget)) {
        return clean(textEdit->toPlainText().left(120));
    }
    if (const auto* combo = qobject_cast<const QComboBox*>(widget)) {
        return clean(combo->currentText());
    }
    if (const auto* group = qobject_cast<const QGroupBox*>(widget)) {
        return clean(group->title());
    }
    if (const auto* tab = qobject_cast<const QTabWidget*>(widget)) {
        return clean(tab->tabText(tab->currentIndex()));
    }
    return {};
}

QString parentInfo(const QWidget* widget) {
    const QWidget* parent = widget ? widget->parentWidget() : nullptr;
    if (!parent) {
        return "null";
    }
    return QString::fromLatin1(parent->metaObject()->className()) + ":" + parent->objectName();
}

void dumpWidget(QTextStream& out, const QWidget* widget, int depth) {
    if (!widget) {
        return;
    }

    const QRect geometry = widget->geometry();
    out << QString(depth * 2, ' ')
        << "class=" << widget->metaObject()->className()
        << " object=" << widget->objectName()
        << " parent=" << parentInfo(widget)
        << " geom=" << geometry.x() << "," << geometry.y() << "," << geometry.width() << "x" << geometry.height()
        << " visible=" << (widget->isVisible() ? 1 : 0)
        << " enabled=" << (widget->isEnabled() ? 1 : 0)
        << " window=" << (widget->isWindow() ? 1 : 0)
        << " title=\"" << clean(widget->windowTitle()) << "\""
        << " text=\"" << safeText(widget) << "\"";

    if (const auto* splitter = qobject_cast<const QSplitter*>(widget)) {
        out << " splitter_sizes=";
        for (int i = 0; i < splitter->count(); ++i) {
            out << (i ? "," : "") << splitter->sizes().value(i);
        }
    }

    if (const auto* view = qobject_cast<const QAbstractItemView*>(widget)) {
        const QAbstractItemModel* model = view->model();
        out << " model=" << (model ? model->metaObject()->className() : "null");
        if (model) {
            out << " rows=" << model->rowCount() << " cols=" << model->columnCount();
            for (int c = 0; c < std::min(model->columnCount(), 6); ++c) {
                out << " header" << c << "=\"" << clean(model->headerData(c, Qt::Horizontal).toString()) << "\"";
            }
            const int rows = std::min(model->rowCount(), 3);
            for (int r = 0; r < rows; ++r) {
                const QModelIndex index = model->index(r, 0);
                out << " row" << r << "=\"" << clean(model->data(index, Qt::DisplayRole).toString()) << "\"";
                for (int role = 0; role <= 20; ++role) {
                    const QString value = clean(model->data(index, role).toString());
                    if (!value.isEmpty()) {
                        out << " role" << role << "=\"" << value << "\"";
                    }
                }
            }
        }
    }

    out << "\n";

    const QObjectList children = widget->children();
    for (QObject* child : children) {
        if (auto* childWidget = qobject_cast<QWidget*>(child)) {
            dumpWidget(out, childWidget, depth + 1);
        }
    }
}

} // namespace

void dumpWidgetTree(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        log("widget dump: cannot open %s", filePath.toUtf8().constData());
        return;
    }

    QTextStream out(&file);
    out << "TS3Stream widget dump\n";
    out << "Qt " << QT_VERSION_STR << "\n";
    out << "allWidgets=" << QApplication::allWidgets().size() << "\n";

    const QWidgetList topLevels = QApplication::topLevelWidgets();
    out << "topLevels=" << topLevels.size() << "\n\n";

    std::vector<QWidget*> sorted(topLevels.begin(), topLevels.end());
    std::sort(sorted.begin(), sorted.end(), [](const QWidget* a, const QWidget* b) {
        const auto areaA = static_cast<qint64>(a->width()) * a->height();
        const auto areaB = static_cast<qint64>(b->width()) * b->height();
        return areaA > areaB;
    });

    for (const QWidget* top : sorted) {
        out << "=== TOP LEVEL ===\n";
        dumpWidget(out, top, 0);
        out << "\n";
    }

    out.flush();
    file.close();
    log("widget dump written to %s", filePath.toUtf8().constData());
}

} // namespace ts3stream::diag
