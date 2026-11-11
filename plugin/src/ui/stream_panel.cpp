#include "stream_panel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QToolButton>
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
        "QToolButton#ts3stream_members_btn { color: #ffffff; background: rgba(255, 255, 255, 38);"
        "  border: 1px solid rgba(255, 255, 255, 90); border-radius: 5px;"
        "  padding: 3px 10px; font-size: 12px; font-weight: 600; }"
        "QToolButton#ts3stream_members_btn:hover { background: rgba(255, 255, 255, 70); }"
        "QWidget#ts3stream_members { background: rgba(18, 18, 18, 210);"
        "  border-top: 2px solid rgba(255, 255, 255, 45); }"
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
    fullscreenButton_ = makeToolButton(QStringLiteral("⛶"), QStringLiteral("На весь экран"), header);
    closeButton_ = makeToolButton(QStringLiteral("✕"), QStringLiteral("Закрыть"), header);

    headerLayout->addWidget(liveBadge_);
    headerLayout->addLayout(titleColumn, 1);
    headerLayout->addWidget(stats_);
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
    QObject::connect(exitFullscreenButton_, &QToolButton::clicked, this, [this] {
        if (onFullscreen_) {
            onFullscreen_();
        }
    });
    QObject::connect(hideMembersButton_, &QToolButton::clicked, this, [this] { hideMembersBar(); });
    QObject::connect(showMembersButton_, &QToolButton::clicked, this, [this] { showMembersBar(); });

    rebuildMembers({});
}

void StreamPanel::setTitle(const QString& title) {
    title_->setText(title);
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
    }
    membersLayout_->addStretch(1);
}

void StreamPanel::setFullscreenMode(bool fullscreen) {
    fullscreenMode_ = fullscreen;
    headerWidget_->setVisible(!fullscreen);
    footerWidget_->setVisible(!fullscreen);

    if (fullscreen) {
        if (!membersHidden_) {
            membersBar_->show();
            membersBar_->raise();
        }
        showMembersButton_->setVisible(membersHidden_);
    } else {
        membersBar_->hide();
        showMembersButton_->hide();
    }
    updateMembersBarGeometry();
}

void StreamPanel::hideMembersBar() {
    membersHidden_ = true;
    membersBar_->hide();
    if (fullscreenMode_) {
        showMembersButton_->show();
        showMembersButton_->raise();
        updateMembersBarGeometry();
    }
}

void StreamPanel::showMembersBar() {
    membersHidden_ = false;
    if (fullscreenMode_) {
        membersBar_->show();
        membersBar_->raise();
    }
    showMembersButton_->hide();
    updateMembersBarGeometry();
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
}

void StreamPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateMembersBarGeometry();
}

} // namespace ts3stream::ui
