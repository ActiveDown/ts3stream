#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

class QHBoxLayout;
class QLabel;
class QToolButton;
class QSlider;
class QTimer;

namespace ts3stream::ui {

struct RoomMember {
    QString name;
    bool talking = false;
    bool self = false;
};

class VideoCanvas : public QWidget {
public:
    explicit VideoCanvas(QWidget* parent = nullptr);

    void setFrame(const QImage& frame);
    void clearFrame();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuildScaled();

    QImage frame_;
    QImage scaled_;
};

class StreamPanel : public QWidget {
public:
    explicit StreamPanel(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setSubtitle(const QString& subtitle);
    void setLive(bool live);
    void setStats(int fps, int width, int height);
    void setAudioStatus(const QString& status);
    void setFrame(const QImage& frame);
    void setMembers(const std::vector<RoomMember>& members);
    void setFullscreenMode(bool fullscreen);
    void setDetachedMode(bool detached);
    void setPlaybackAudio(int volumePercent, bool muted);

    void setOnClose(std::function<void()> callback);
    void setOnFullscreen(std::function<void()> callback);
    void setOnDetach(std::function<void()> callback);
    void setOnPlaybackAudioChanged(std::function<void(int, bool)> callback);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildMembers(const std::vector<RoomMember>& members);
    void updateMembersBarGeometry();
    void hideMembersBar();
    void showMembersBar();
    void trackControlsActivity(QWidget* widget);
    void showFullscreenControls();
    void hideIdleControls();

    QLabel* liveBadge_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* stats_ = nullptr;
    QLabel* audio_ = nullptr;
    QToolButton* fullscreenButton_ = nullptr;
    QToolButton* detachButton_ = nullptr;
    QToolButton* closeButton_ = nullptr;
    VideoCanvas* canvas_ = nullptr;

    QWidget* headerWidget_ = nullptr;
    QWidget* footerWidget_ = nullptr;
    QWidget* audioControls_ = nullptr;
    QToolButton* muteButton_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QLabel* volumeLabel_ = nullptr;
    int volumePercent_ = 100;
    bool muted_ = false;
    QWidget* membersBar_ = nullptr;
    QHBoxLayout* membersLayout_ = nullptr;
    QToolButton* hideMembersButton_ = nullptr;
    QToolButton* showMembersButton_ = nullptr;
    QToolButton* exitFullscreenButton_ = nullptr;

    std::vector<RoomMember> members_;
    bool fullscreenMode_ = false;
    bool membersHidden_ = false;
    QTimer* controlsIdleTimer_ = nullptr;
    bool controlsHidden_ = false;
    QPoint lastMousePosition_;

    std::function<void()> onClose_;
    std::function<void()> onFullscreen_;
    std::function<void()> onDetach_;
    std::function<void(int, bool)> onPlaybackAudioChanged_;
};

} // namespace ts3stream::ui
