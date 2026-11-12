#include "source_picker.h"

#include "../media/window_enumerator.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <vector>

namespace ts3stream::ui {
namespace {

QString monitorTitle(const media::MonitorInfo& monitor, int index) {
    const int width = monitor.rect.right - monitor.rect.left;
    const int height = monitor.rect.bottom - monitor.rect.top;
    QString title = QStringLiteral("Экран %1 — %2×%3").arg(index + 1).arg(width).arg(height);
    if (monitor.primary) {
        title += QStringLiteral(" (основной)");
    }
    return title;
}

class SourcePickerDialog : public QDialog {
public:
    explicit SourcePickerDialog(QWidget* parent) : QDialog(parent) {
        setWindowTitle(QStringLiteral("Начать стрим"));
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        resize(760, 640);

        setStyleSheet(QStringLiteral(
            "QDialog { background: #1e1e1e; color: #e0e0e0; }"
            "QLabel, QRadioButton, QGroupBox { color: #e0e0e0; }"
            "QGroupBox { border: 1px solid #3a3a3a; border-radius: 4px; margin-top: 10px; padding-top: 8px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"
            "QTabWidget::pane { border: 1px solid #3a3a3a; background: #232323; }"
            "QTabBar::tab { background: #2a2a2a; color: #d0d0d0; padding: 6px 14px; }"
            "QTabBar::tab:selected { background: #3a3a3a; }"
            "QListWidget { background: #232323; color: #e0e0e0; border: none; }"
            "QListWidget::item { padding: 6px; }"
            "QListWidget::item:selected { background: #3d5a80; }"
            "QComboBox, QSpinBox { background: #2a2a2a; color: #e0e0e0; border: 1px solid #4a4a4a; padding: 3px 6px; }"
            "QComboBox QAbstractItemView { background: #232323; color: #e0e0e0; selection-background-color: #3d5a80; }"
            "QPushButton { background: #333333; color: #e0e0e0; border: 1px solid #4a4a4a;"
            "  padding: 5px 14px; border-radius: 3px; }"
            "QPushButton:hover { background: #404040; }"
        ));

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(10);

        auto* hint = new QLabel(QStringLiteral(
            "Выберите экран или окно приложения, звук, разрешение и частоту кадров."), this);
        hint->setWordWrap(true);
        root->addWidget(hint);

        auto* qualityGroup = new QGroupBox(QStringLiteral("Качество видео"), this);
        auto* qualityLayout = new QFormLayout(qualityGroup);
        qualityLayout->setContentsMargins(10, 14, 10, 10);
        qualityLayout->setSpacing(8);
        resolution_ = new QComboBox(qualityGroup);
        resolution_->setObjectName(QStringLiteral("ts3stream_resolution"));
        resolution_->addItem(QStringLiteral("720p — 1280×720"), 720);
        resolution_->addItem(QStringLiteral("1080p — 1920×1080"), 1080);
        resolution_->addItem(QStringLiteral("1440p — 2560×1440"), 1440);
        resolution_->setCurrentIndex(1);
        resolution_->setToolTip(QStringLiteral("Максимальный размер кадра. Пропорции сохраняются, маленькие окна не увеличиваются."));
        qualityLayout->addRow(QStringLiteral("Разрешение:"), resolution_);
        fps_ = new QComboBox(qualityGroup);
        fps_->setObjectName(QStringLiteral("ts3stream_fps"));
        fps_->addItem(QStringLiteral("30 FPS — стабильно"), 30);
        fps_->addItem(QStringLiteral("60 FPS — плавно"), 60);
        fps_->addItem(QStringLiteral("Авто — до 60 FPS по каналу"), 60);
        fps_->setCurrentIndex(0);
        qualityLayout->addRow(QStringLiteral("Частота кадров:"), fps_);
        root->addWidget(qualityGroup);

        tabs_ = new QTabWidget(this);
        root->addWidget(tabs_, 1);

        screens_ = new QListWidget(tabs_);
        windows_ = new QListWidget(tabs_);
        tabs_->addTab(screens_, QStringLiteral("Экраны"));
        tabs_->addTab(windows_, QStringLiteral("Окна приложений"));

        auto* audioGroup = new QGroupBox(QStringLiteral("Звук"), this);
        auto* audioLayout = new QVBoxLayout(audioGroup);
        audioLayout->setContentsMargins(10, 14, 10, 10);
        audioLayout->setSpacing(6);

        audioApp_ = new QRadioButton(QStringLiteral("Звук приложения (для окна)"), audioGroup);
        audioApp_->setEnabled(false);
        audioDevice_ = new QRadioButton(QStringLiteral("Системный звук (кроме TeamSpeak)"), audioGroup);
        audioNone_ = new QRadioButton(QStringLiteral("Без звука"), audioGroup);

        auto* audioRadioGroup = new QButtonGroup(this);
        audioRadioGroup->addButton(audioApp_);
        audioRadioGroup->addButton(audioDevice_);
        audioRadioGroup->addButton(audioNone_);
        audioRadioGroup->setExclusive(true);

        audioLayout->addWidget(audioApp_);
        audioLayout->addWidget(audioDevice_);
        audioLayout->addWidget(audioNone_);
        root->addWidget(audioGroup);

        auto* bitrateLayout = new QFormLayout();
        bitrate_ = new QSpinBox(this);
        bitrate_->setObjectName(QStringLiteral("ts3stream_bitrate"));
        bitrate_->setRange(media::kMinVideoBitrateKbps, media::kMaxVideoBitrateKbps);
        bitrate_->setSingleStep(1000);
        bitrate_->setValue(media::kDefaultVideoBitrateKbps);
        bitrate_->setSuffix(QStringLiteral(" Кбит/с"));
        bitrate_->setToolTip(QStringLiteral(
            "Верхний предел видеобитрейта. При ухудшении связи скорость снижается автоматически. "
            "Применяется при запуске этого стрима."));
        bitrateLayout->addRow(QStringLiteral("Битрейт видео:"), bitrate_);
        root->addLayout(bitrateLayout);

        auto* bottom = new QHBoxLayout();
        auto* refresh = new QPushButton(QStringLiteral("Обновить"), this);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Начать стрим"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
        bottom->addWidget(refresh);
        bottom->addStretch(1);
        bottom->addWidget(buttons);
        root->addLayout(bottom);

        connect(refresh, &QPushButton::clicked, this, [this] { reload(); });
        connect(tabs_, &QTabWidget::currentChanged, this, [this] { updateDefaults(); });
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (!selected().has_value()) {
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(windows_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
            if (selected().has_value()) {
                accept();
            }
        });
        connect(screens_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
            if (selected().has_value()) {
                accept();
            }
        });

        reload();
        updateDefaults();
    }

    std::optional<CaptureSource> selected() const {
        CaptureSource source;
        if (tabs_->currentWidget() == screens_) {
            const auto* item = screens_->currentItem();
            if (!item) {
                return std::nullopt;
            }
            const int index = item->data(Qt::UserRole).toInt();
            if (index < 0 || index >= static_cast<int>(monitors_.size())) {
                return std::nullopt;
            }
            const auto& monitor = monitors_[index];
            source.type = CaptureSource::Type::Monitor;
            source.monitor = monitor.handle;
            source.title = monitorTitle(monitor, index);
        } else {
            const auto* item = windows_->currentItem();
            if (!item) {
                return std::nullopt;
            }
            const int index = item->data(Qt::UserRole).toInt();
            if (index < 0 || index >= static_cast<int>(windowsList_.size())) {
                return std::nullopt;
            }
            const auto& window = windowsList_[index];
            source.type = CaptureSource::Type::Window;
            source.window = window.hwnd;
            source.title = QString::fromStdWString(window.title);
            source.processId = window.processId;
            source.processName = QString::fromStdWString(window.processName);
            source.processPath = QString::fromStdWString(window.processPath);
        }

        if (audioApp_->isChecked() && source.type == CaptureSource::Type::Window) {
            source.audioType = CaptureSource::AudioType::Process;
            source.audioDeviceName = QStringLiteral("приложение");
        } else if (audioDevice_->isChecked()) {
            // System audio is always captured with process-loopback excluding
            // the current TeamSpeak process tree; raw device loopback must
            // never be used for streaming because it would capture TS3 itself.
            source.audioType = CaptureSource::AudioType::Device;
            source.audioDeviceName = QStringLiteral("система (без TeamSpeak)");
        } else {
            source.audioType = CaptureSource::AudioType::None;
        }

        source.autoFps = fps_->currentIndex() == 2;
        source.resolutionHeight = resolution_->currentData().toInt();
        source.videoBitrateKbps = bitrate_->value();
        source.fps = source.autoFps ? 60 : fps_->currentData().toInt();
        if (source.fps <= 0) {
            source.fps = 30;
        }

        return source;
    }

private:
    void updateDefaults() {
        const bool windowTab = tabs_->currentWidget() == windows_;
        audioApp_->setEnabled(windowTab);
        if (windowTab) {
            audioApp_->setChecked(true);
        } else {
            audioDevice_->setChecked(true);
        }
    }

    void reload() {
        monitors_ = media::enumerateMonitors();
        windowsList_ = media::enumerateWindows();

        screens_->clear();
        for (int i = 0; i < static_cast<int>(monitors_.size()); ++i) {
            auto* item = new QListWidgetItem(QStringLiteral("🖥  ") + monitorTitle(monitors_[i], i), screens_);
            item->setData(Qt::UserRole, i);
        }
        if (screens_->count() > 0) {
            screens_->setCurrentRow(0);
        }

        windows_->clear();
        QFileIconProvider iconProvider;
        for (int i = 0; i < static_cast<int>(windowsList_.size()); ++i) {
            const auto& window = windowsList_[i];
            QString text = QString::fromStdWString(window.title);
            if (!window.processName.empty()) {
                text += QStringLiteral("\n") + QString::fromStdWString(window.processName);
            }
            auto* item = new QListWidgetItem(text, windows_);
            item->setData(Qt::UserRole, i);
            if (!window.processPath.empty()) {
                const QIcon icon = iconProvider.icon(QFileInfo(QString::fromStdWString(window.processPath)));
                if (!icon.isNull()) {
                    item->setIcon(icon);
                }
            }
        }
        if (windows_->count() > 0) {
            windows_->setCurrentRow(0);
        }

    }

    QTabWidget* tabs_ = nullptr;
    QListWidget* screens_ = nullptr;
    QListWidget* windows_ = nullptr;
    QRadioButton* audioApp_ = nullptr;
    QRadioButton* audioDevice_ = nullptr;
    QRadioButton* audioNone_ = nullptr;
    QComboBox* fps_ = nullptr;
    QComboBox* resolution_ = nullptr;
    QSpinBox* bitrate_ = nullptr;

    std::vector<media::MonitorInfo> monitors_;
    std::vector<media::WindowInfo> windowsList_;
};

} // namespace

std::optional<CaptureSource> pickCaptureSource(QWidget* parent) {
    SourcePickerDialog dialog(parent);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return dialog.selected();
}

} // namespace ts3stream::ui
