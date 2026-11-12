#include "stream_panel.h"
#include "media/playback_gain.h"

#include <QHBoxLayout>
#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QTimer>
#include <QCloseEvent>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QToolButton>
#include <QSlider>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace ts3stream::ui {
namespace {

QLabel* makeLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    return label;
}

QToolButton* makeToolButton(const QString& text, const QString& tooltip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tooltip);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

} // namespace

VideoCanvas::VideoCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
}

void VideoCanvas::setFrame(const QImage& frame) {
    frame_ = frame;
    rebuildScaled();
    update();
}

void VideoCanvas::clearFrame() {
    frame_ = QImage();
    scaled_ = QImage();
    update();
}

void VideoCanvas::rebuildScaled() {
    if (frame_.isNull() || width() <= 0 || height() <= 0) {
        scaled_ = QImage();
        return;
    }
    scaled_ = frame_.scaled(size(), Qt::KeepAspectRatio, Qt::FastTransformation);
}

void VideoCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    rebuildScaled();
}

void VideoCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(12, 12, 12));

    if (scaled_.isNull()) {
        painter.setPen(QColor(150, 150, 150));
        QFont font = painter.font();
        font.setPointSize(11);
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Ожидание кадра…"));
        return;
    }

    const int x = (width() - scaled_.width()) / 2;
    const int y = (height() - scaled_.height()) / 2;
    painter.drawImage(x, y, scaled_);
}

StreamPanel::StreamPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("ts3stream_panel");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "#ts3stream_panel { background: #1b1b1b; }"
        "QLabel#ts3stream_title { color: #f0f0f0; font-size: 14px; font-weight: 600; }"
        "QLabel#ts3stream_subtitle { color: #9a9a9a; font-size: 11px; }"
        "QLabel#ts3stream_stats { color: #9a9a9a; font-size: 11px; }"
        "QLabel#ts3stream_audio { color: #9a9a9a; font-size: 11px; }"
        "QLabel#ts3stream_live { color: white; background: #d93025; border-radius: 3px;"
        "  padding: 2px 6px; font-size: 10px; font-weight: 700; }"
        "QLabel#ts3stream_members_title { color: #cfcfcf; font-size: 11px; font-weight: 600; }"
        "QToolButton { color: #d0d0d0; border: none; padding: 2px 6px; }"
        "QToolButton:hover { background: #333333; border-radius: 3px; }"
        "QToolButton#ts3stream_members_btn { color: #f2f2f2; background: rgba(255, 255, 255, 22);"
        "  border: 1px solid rgba(255, 255, 255, 55); border-radius: 5px;"
        "  padding: 3px 10px; font-size: 12px; font-weight: 600; }"
        "QToolButton#ts3stream_members_btn:hover { background: rgba(255, 255, 255, 45); }"
        "QWidget#ts3stream_members { background: rgba(18, 18, 18, 150);"
        "  border-top: 2px solid rgba(255, 255, 255, 32); }"
    ));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QWidget(this);
    headerWidget_ = header;
    header->setObjectName("ts3stream_header");
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setStyleSheet("QWidget#ts3stream_header { background: #242424; }");
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 6, 8, 6);
    headerLayout->setSpacing(8);

    liveBadge_ = makeLabel(QStringLiteral("LIVE"), QStringLiteral("ts3stream_live"), header);
    liveBadge_->hide();

    auto* titleColumn = new QVBoxLayout();
    titleColumn->setContentsMargins(0, 0, 0, 0);
    titleColumn->setSpacing(1);
    title_ = makeLabel(QStringLiteral("Стрим"), QStringLiteral("ts3stream_title"), header);
    subtitle_ = makeLabel(QString(), QStringLiteral("ts3stream_subtitle"), header);
    titleColumn->addWidget(title_);
    titleColumn->addWidget(subtitle_);

    stats_ = makeLabel(QString(), QStringLiteral("ts3stream_stats"), header);
    detachButton_ = makeToolButton(QStringLiteral("Открепить"),
                                   QStringLiteral("Открыть стрим в отдельном окне"), header);
    detachButton_->setObjectName(QStringLiteral("ts3stream_detach"));
    fullscreenButton_ = makeToolButton(QStringLiteral("⛶"), QStringLiteral("На весь экран"), header);
    closeButton_ = makeToolButton(QStringLiteral("✕"), QStringLiteral("Закрыть"), header);

    headerLayout->addWidget(liveBadge_);
    headerLayout->addLayout(titleColumn, 1);
    headerLayout->addWidget(stats_);
    headerLayout->addWidget(detachButton_);
    headerLayout->addWidget(fullscreenButton_);
    headerLayout->addWidget(closeButton_);

    canvas_ = new VideoCanvas(this);

    auto* footer = new QWidget(this);
    footerWidget_ = footer;
    footer->setObjectName("ts3stream_footer");
    footer->setAttribute(Qt::WA_StyledBackground, true);
    footer->setStyleSheet("QWidget#ts3stream_footer { background: #242424; }");
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(10, 4, 10, 4);
    footerLayout->setSpacing(8);
    audio_ = makeLabel(QStringLiteral("Звук: не выбран"), QStringLiteral("ts3stream_audio"), footer);
    footerLayout->addWidget(audio_, 1);

    // One control widget follows the viewer into fullscreen, independently
    // of the members bar so muting is still available when members are hidden.
    audioControls_ = new QWidget(footer);
    audioControls_->setObjectName(QStringLiteral("ts3stream_playback_audio"));
    audioControls_->setAttribute(Qt::WA_StyledBackground, true);
    audioControls_->setStyleSheet(QStringLiteral(
        "QWidget#ts3stream_playback_audio { background: rgba(24,24,24,210); border-radius: 5px; }"
        "QLabel { color: #eeeeee; }"));
    auto* audioLayout = new QHBoxLayout(audioControls_);
    audioLayout->setContentsMargins(8, 3, 8, 3);
    audioLayout->setSpacing(6);
    muteButton_ = makeToolButton(QStringLiteral("Звук"), QStringLiteral("Выключить звук стрима"), audioControls_);
    muteButton_->setObjectName(QStringLiteral("ts3stream_mute"));
    muteButton_->setCheckable(true);
    muteButton_->setMinimumWidth(94);
    volumeSlider_ = new QSlider(Qt::Horizontal, audioControls_);
    volumeSlider_->setObjectName(QStringLiteral("ts3stream_volume"));
    volumeSlider_->setRange(0, media::kMaxPlaybackVolumePercent);
    volumeSlider_->setValue(100);
    volumeSlider_->setFixedWidth(120);
    volumeSlider_->setToolTip(QStringLiteral("Громкость стрима: до 125% (не влияет на голоса TeamSpeak)"));
    volumeSlider_->setAccessibleName(QStringLiteral("Громкость стрима"));
    volumeLabel_ = new QLabel(QStringLiteral("100%"), audioControls_);
    volumeLabel_->setMinimumWidth(38);
    audioLayout->addWidget(muteButton_);
    audioLayout->addWidget(volumeSlider_);
    audioLayout->addWidget(volumeLabel_);
    footerLayout->addWidget(audioControls_);
    QObject::connect(muteButton_, &QToolButton::toggled, this, [this](bool muted) {
        setPlaybackAudio(volumePercent_, muted);
        if (onPlaybackAudioChanged_) onPlaybackAudioChanged_(volumePercent_, muted_);
    });
    QObject::connect(volumeSlider_, &QSlider::valueChanged, this, [this](int volume) {
        setPlaybackAudio(volume, muted_);
        if (onPlaybackAudioChanged_) onPlaybackAudioChanged_(volumePercent_, muted_);
    });

    root->addWidget(header);
    root->addWidget(canvas_, 1);
    root->addWidget(footer);

    // Semi-transparent participant bar, shown over the video in fullscreen.
    membersBar_ = new QWidget(this);
    membersBar_->setObjectName("ts3stream_members");
    membersBar_->setAttribute(Qt::WA_StyledBackground, true);
    auto* membersBarLayout = new QHBoxLayout(membersBar_);
    membersBarLayout->setContentsMargins(14, 3, 6, 3);
    membersBarLayout->setSpacing(10);

    auto* membersTitle = makeLabel(QStringLiteral("В комнате:"), QStringLiteral("ts3stream_members_title"), membersBar_);
    membersBarLayout->addWidget(membersTitle);

    membersLayout_ = new QHBoxLayout();
    membersLayout_->setContentsMargins(0, 0, 0, 0);
    membersLayout_->setSpacing(14);
    membersBarLayout->addLayout(membersLayout_, 1);

    exitFullscreenButton_ = makeToolButton(QStringLiteral("Выйти"), QStringLiteral("Выйти из полноэкранного режима"), membersBar_);
    exitFullscreenButton_->setObjectName("ts3stream_members_btn");
    exitFullscreenButton_->setMinimumSize(78, 30);
    hideMembersButton_ = makeToolButton(QStringLiteral("Скрыть"), QStringLiteral("Скрыть панель участников"), membersBar_);
    hideMembersButton_->setObjectName("ts3stream_members_btn");
    hideMembersButton_->setMinimumSize(78, 30);
    membersBarLayout->addWidget(exitFullscreenButton_);
    membersBarLayout->addWidget(hideMembersButton_);
    membersBar_->hide();

    showMembersButton_ = new QToolButton(this);
    showMembersButton_->setObjectName("ts3stream_members_btn");
    showMembersButton_->setText(QStringLiteral("▲ Участники"));
    showMembersButton_->setToolTip(QStringLiteral("Показать участников"));
    showMembersButton_->setCursor(Qt::PointingHandCursor);
    showMembersButton_->setMinimumSize(118, 30);
    showMembersButton_->hide();

    QObject::connect(closeButton_, &QToolButton::clicked, this, [this] {
        if (onClose_) {
            onClose_();
        }
    });
    QObject::connect(fullscreenButton_, &QToolButton::clicked, this, [this] {
        if (onFullscreen_) {
            onFullscreen_();
        }
    });
    QObject::connect(detachButton_, &QToolButton::clicked, this, [this] {
        if (onDetach_) onDetach_();
    });
    QObject::connect(exitFullscreenButton_, &QToolButton::clicked, this, [this] {
        if (onFullscreen_) {
            onFullscreen_();
        }
    });
    QObject::connect(hideMembersButton_, &QToolButton::clicked, this, [this] { hideMembersBar(); });
    QObject::connect(showMembersButton_, &QToolButton::clicked, this, [this] { showMembersBar(); });

    controlsIdleTimer_ = new QTimer(this);
    controlsIdleTimer_->setObjectName(QStringLiteral("ts3stream_controls_idle"));
    controlsIdleTimer_->setSingleShot(true);
    controlsIdleTimer_->setInterval(3000);
    QObject::connect(controlsIdleTimer_, &QTimer::timeout, this, [this] { hideIdleControls(); });
    rebuildMembers({});
    trackControlsActivity(this);
}

void StreamPanel::setTitle(const QString& title) {
    title_->setText(title);
    setWindowTitle(title + QStringLiteral(" — TS3Stream"));
}

void StreamPanel::setSubtitle(const QString& subtitle) {
    subtitle_->setText(subtitle);
}

void StreamPanel::setLive(bool live) {
    liveBadge_->setVisible(live);
}

void StreamPanel::setStats(int fps, int width, int height) {
    if (fps <= 0 && width <= 0) {
        stats_->clear();
        return;
    }
    stats_->setText(QStringLiteral("%1 FPS · %2×%3").arg(fps).arg(width).arg(height));
}

void StreamPanel::setAudioStatus(const QString& status) {
    audio_->setText(QStringLiteral("Звук: %1").arg(status));
}

void StreamPanel::setFrame(const QImage& frame) {
    canvas_->setFrame(frame);
}

void StreamPanel::setOnClose(std::function<void()> callback) {
    onClose_ = std::move(callback);
}

void StreamPanel::setOnFullscreen(std::function<void()> callback) {
    onFullscreen_ = std::move(callback);
}

void StreamPanel::setOnDetach(std::function<void()> callback) {
    onDetach_ = std::move(callback);
}

void StreamPanel::setOnPlaybackAudioChanged(std::function<void(int, bool)> callback) {
    onPlaybackAudioChanged_ = std::move(callback);
}

void StreamPanel::setPlaybackAudio(int volumePercent, bool muted) {
    volumePercent_ = qBound(0, volumePercent, media::kMaxPlaybackVolumePercent);
    muted_ = muted;
    const QSignalBlocker muteBlocker(muteButton_);
    const QSignalBlocker volumeBlocker(volumeSlider_);
    muteButton_->setChecked(muted);
    muteButton_->setText(muted ? QStringLiteral("Без звука") : QStringLiteral("Звук"));
    muteButton_->setToolTip(muted ? QStringLiteral("Включить звук стрима")
                                 : QStringLiteral("Выключить звук стрима"));
    volumeSlider_->setValue(volumePercent_);
    volumeLabel_->setText(QStringLiteral("%1%").arg(volumePercent_));
}

void StreamPanel::setDetachedMode(bool detached) {
    detachButton_->setText(detached ? QStringLiteral("Встроить") : QStringLiteral("Открепить"));
    detachButton_->setToolTip(detached ? QStringLiteral("Вернуть стрим в окно TeamSpeak")
                                     : QStringLiteral("Открыть стрим в отдельном окне"));
}

void StreamPanel::closeEvent(QCloseEvent* event) {
    // Native title-bar X / Alt+F4 must stop viewing just like our close button.
    // StreamUi schedules deletion; do not leave a hidden viewer playing audio.
    event->accept();
    if (onClose_) onClose_();
}

void StreamPanel::setMembers(const std::vector<RoomMember>& members) {
    members_ = members;
    rebuildMembers(members_);
}

void StreamPanel::rebuildMembers(const std::vector<RoomMember>& members) {
    while (QLayoutItem* item = membersLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            delete widget;
        }
        delete item;
    }

    if (members.empty()) {
        auto* empty = new QLabel(QStringLiteral("Нет данных — подключись к серверу TS3"), membersBar_);
        empty->setStyleSheet("color: #8a8a8a; font-size: 11px;");
        membersLayout_->addWidget(empty);
        trackControlsActivity(empty);
        membersLayout_->addStretch(1);
        return;
    }

    for (const RoomMember& member : members) {
        const QString dotColor = member.talking ? QStringLiteral("#3ddc84") : QStringLiteral("#777777");
        const QString textColor = member.talking ? QStringLiteral("#ffffff") : QStringLiteral("#b8b8b8");
        const QString weight = member.talking ? QStringLiteral("600") : QStringLiteral("400");
        QString text = QStringLiteral("<span style='color:%1'>●</span> "
                                     "<span style='color:%2;font-weight:%3'>%4%5</span>")
                           .arg(dotColor)
                           .arg(textColor)
                           .arg(weight)
                           .arg(member.name.toHtmlEscaped())
                           .arg(member.self ? QStringLiteral(" (ты)") : QString());
        auto* label = new QLabel(text, membersBar_);
        label->setTextFormat(Qt::RichText);
        label->setStyleSheet("font-size: 12px;");
        membersLayout_->addWidget(label);
        trackControlsActivity(label);
    }
    membersLayout_->addStretch(1);
}

void StreamPanel::setFullscreenMode(bool fullscreen) {
    fullscreenMode_ = fullscreen;
    controlsIdleTimer_->stop();
    controlsHidden_ = false;
    canvas_->unsetCursor();
    if (fullscreen && audioControls_->parentWidget() != this) {
        audioControls_->setParent(this);
        audioControls_->show();
    } else if (!fullscreen && audioControls_->parentWidget() != footerWidget_) {
        audioControls_->setParent(footerWidget_);
        footerWidget_->layout()->addWidget(audioControls_);
        audioControls_->show();
    }
    headerWidget_->setVisible(!fullscreen);
    footerWidget_->setVisible(!fullscreen);

    if (fullscreen) {
        lastMousePosition_ = QCursor::pos();
        showFullscreenControls();
    } else {
        audioControls_->show();
        membersBar_->hide();
        showMembersButton_->hide();
    }
    updateMembersBarGeometry();
}

void StreamPanel::hideMembersBar() {
    membersHidden_ = true;
    membersBar_->hide();
    if (fullscreenMode_) {
        showFullscreenControls();
    }
    updateMembersBarGeometry();
}

void StreamPanel::showMembersBar() {
    membersHidden_ = false;
    if (fullscreenMode_) {
        showFullscreenControls();
    }
    showMembersButton_->hide();
    updateMembersBarGeometry();
}

void StreamPanel::trackControlsActivity(QWidget* widget) {
    // Observe only plugin-owned widgets, never TS3's widget tree.
    widget->setMouseTracking(true);
    widget->installEventFilter(this);
    for (auto* child : widget->findChildren<QWidget*>()) {
        child->setMouseTracking(true);
        child->installEventFilter(this);
    }
}

bool StreamPanel::eventFilter(QObject* watched, QEvent* event) {
    if (fullscreenMode_) {
        bool activity = false;
        if (event->type() == QEvent::MouseMove) {
            const QPoint position = static_cast<QMouseEvent*>(event)->globalPos();
            activity = position != lastMousePosition_;
            lastMousePosition_ = position;
        } else {
            activity = event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
                       event->type() == QEvent::Wheel || event->type() == QEvent::KeyPress;
        }
        if (activity) showFullscreenControls();
    }
    return QWidget::eventFilter(watched, event);
}

void StreamPanel::showFullscreenControls() {
    if (!fullscreenMode_) return;
    controlsHidden_ = false;
    canvas_->unsetCursor();
    audioControls_->show();
    membersBar_->setVisible(!membersHidden_);
    showMembersButton_->setVisible(membersHidden_);
    updateMembersBarGeometry();
    controlsIdleTimer_->start();
}

void StreamPanel::hideIdleControls() {
    if (!fullscreenMode_ || controlsHidden_) return;
    const QPoint position = mapFromGlobal(QCursor::pos());
    const auto hovered = [&](QWidget* widget) {
        return widget->isVisible() && widget->geometry().contains(position);
    };
    if (QApplication::mouseButtons() != Qt::NoButton || volumeSlider_->isSliderDown() ||
        hovered(audioControls_) || hovered(membersBar_) || hovered(showMembersButton_)) {
        controlsIdleTimer_->start();
        return;
    }
    controlsHidden_ = true;
    audioControls_->hide();
    membersBar_->hide();
    showMembersButton_->hide();
    canvas_->setCursor(Qt::BlankCursor);
}

void StreamPanel::updateMembersBarGeometry() {
    constexpr int kBarHeight = 44;
    if (membersBar_) {
        membersBar_->setGeometry(0, height() - kBarHeight, width(), kBarHeight);
        if (membersBar_->isVisible()) {
            membersBar_->raise();
        }
    }
    if (showMembersButton_) {
        showMembersButton_->setGeometry(width() - 150, height() - 43, 138, 32);
        if (showMembersButton_->isVisible()) {
            showMembersButton_->raise();
        }
    }
    if (fullscreenMode_ && audioControls_) {
        const QSize size = audioControls_->sizeHint();
        const int bottomInset = membersHidden_ ? 10 : kBarHeight + 10;
        audioControls_->setGeometry(10, height() - bottomInset - size.height(),
                                    size.width(), size.height());
        audioControls_->raise();
    }
}

void StreamPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateMembersBarGeometry();
}

} // namespace ts3stream::ui
