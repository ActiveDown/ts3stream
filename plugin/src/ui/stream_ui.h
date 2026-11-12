#pragma once

#include "frame_processor.h"
#include "control_client.h"
#include "live_badge_delegate.h"
#include "media/audio_pipe_client.h"
#include "media/audio_renderer.h"
#include "media/livekit_engine.h"
#include "media/wgc_capture.h"
#include "source_picker.h"
#include "ts3_functions.h"

#include <QImage>
#include <QByteArray>
#include <QObject>
#include <QPointer>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

class QAction;
class QMenu;
class QTimer;
class QToolBar;
class QToolButton;
class QTreeView;
class QWidget;

namespace ts3stream::ui {

class StreamPanel;
class ActiveStreamsButton;

class StreamUi : public QObject {
public:
    static StreamUi& instance();

    void initialize();
    void shutdown();
    bool isInitialized() const { return initialized_; }

    void setTs3Functions(const TS3Functions& functions);
    void onTalkStatusChanged(uint64 serverConnectionHandlerID, anyID clientID, bool talking);
    void openStreamForClient(uint64 serverConnectionHandlerID, anyID clientID);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct CaptureState;

    StreamUi() = default;

    void startFromPicker();
    void startCapture(const CaptureSource& source);
    void beginCapture(const CaptureSource& source, const LiveKitToken& token,
                      const QString& room, const QString& identity, const QString& nickname,
                      int channelId, const QString& serverUid);
    void startViewer();
    void startViewerForStream(const ControlStream& stream);
    void startViewerForRoom(QString room, QString targetIdentity, QString nickname);
    bool roomContext(QString& room, QString& serverUid, int& channelId,
                     QString& identity, QString& nickname) const;
    bool roomForChannel(uint64 serverConnectionHandlerID, int channelId,
                        QString& room, QString& serverUid) const;
    void ensureWatchButton();
    void updateWatchButton();
    void startPanelTimer();
    void updateRoomMembers();
    void updateStreamButtonState();
    void installLiveBadgeDelegate();
    void refreshLiveStreams();
    void tryAutoConfigure();
    void processVideoFrame(const std::shared_ptr<CaptureState>& state,
                           const media::CapturedVideoFrame& frame);
    void stopCapture(bool forShutdown = false);
    void stopViewer(bool forShutdown = false);
    void closeEndedViewer(int generation);
    void ensurePanel();
    void restoreInfoPanel();
    void toggleFullscreen();
    void toggleDetached();
    void refreshPanelGeometry();
    QWidget* findMainWindow() const;
    QWidget* findVisibleInfoFrame(QWidget* mainWindow) const;
    QWidget* findVisibleServerTree(QWidget* mainWindow) const;

    struct CaptureState {
        std::atomic<bool> alive{true};
        std::atomic<int> frames{0};
        std::atomic<int64_t> lastVideoUs{0};
        std::chrono::steady_clock::time_point started{};
        QPointer<StreamPanel> panel;
        QString title;
        int fps = 30;
        bool autoFps = false;
        int videoBitrateKbps = media::kDefaultVideoBitrateKbps;
        QSize resolutionBounds{1920, 1080};
        std::shared_ptr<media::LiveKitEngine> livekit;
        std::atomic<bool> livekitVideoReady{false};

        std::mutex frameMutex;
        QImage latestFrame;
        int latestFps = 0;
        int latestWidth = 0;
        int latestHeight = 0;
    };

    struct AudioState {
        std::atomic<bool> alive{true};
        std::atomic<int64_t> lastUpdateMs{0};
        std::atomic<int> logCounter{0};
        QPointer<StreamPanel> panel;
        QString sourceName;
        std::shared_ptr<media::LiveKitEngine> livekit;
        std::atomic<bool> livekitAudioReady{false};
    };

    bool initialized_ = false;
    bool connected_ = false;
    bool publishing_ = false;
    QSize lastPanelSize_;
    QPointer<QWidget> mainWindow_;
    QPointer<QToolBar> toolBar_;
    QPointer<QToolButton> streamButton_;
    QPointer<ActiveStreamsButton> activeStreamsButton_;
    QPointer<QAction> activeStreamsAction_;
    uint64 streamListHandler_ = 0;
    QPointer<QMenu> streamMenu_;
    QPointer<QAction> startAction_;
    QPointer<QAction> stopAction_;
    QPointer<QWidget> infoFrame_;
    QPointer<QWidget> infoWrapper_;
    QPointer<QWidget> serverTree_;

    QPointer<StreamPanel> panel_;
    bool fullscreen_ = false;
    bool detached_ = false;
    QByteArray detachedGeometry_;
    std::unique_ptr<media::WgcCapture> capture_;
    std::shared_ptr<CaptureState> captureState_;
    std::shared_ptr<CaptureState> viewerState_;
    std::shared_ptr<FrameProcessor> frameProcessor_;
    std::unique_ptr<media::RemoteAudioCapture> audioCapture_;
    std::shared_ptr<AudioState> audioState_;
    std::shared_ptr<media::LiveKitEngine> livekit_;
    std::shared_ptr<media::LiveKitEngine> viewerLivekit_;
    std::shared_ptr<media::AudioRenderer> audioRenderer_;
    int playbackVolume_ = 100;
    bool playbackMuted_ = false;
    QTimer* watchdog_ = nullptr;
    QTimer* panelTimer_ = nullptr;
    QTimer* membersTimer_ = nullptr;
    QTimer* heartbeatTimer_ = nullptr;
    QTimer* streamsTimer_ = nullptr;
    QPointer<QTimer> autostartViewerTimer_;

    std::unique_ptr<ControlClient> control_;
    QString activeRoom_;
    QString activeIdentity_;
    QString activeNickname_;
    QString activeServerUid_;
    int activeChannelId_ = 0;
    int captureGeneration_ = 0;
    int viewerGeneration_ = 0;
    QString viewerRoom_;
    QString viewerTargetIdentity_;
    int autoConfigAttempts_ = 0;
    bool autoConfigInProgress_ = false;
    bool autoConfigDone_ = false;
    std::chrono::steady_clock::time_point nextAutoConfigAttempt_{};
    bool streamsRequestActive_ = false;

    QPointer<QTreeView> treeView_;
    QPointer<QWidget> treeViewport_;
    QPointer<LiveBadgeDelegate> liveDelegate_;
    QHash<QString, LiveStreamInfo> liveStreams_;
    QPointer<QToolButton> watchButton_;
    LiveStreamInfo watchStream_;

    // TS3/Qt teardown is hostile: destroying WGC/COM objects after the client
    // starts shutting down produced a ucrtbase fail-fast. Stop the workers, but
    // keep their objects alive until the process exits.
    std::vector<std::unique_ptr<media::WgcCapture>> retiredCaptures_;
    std::vector<std::unique_ptr<media::RemoteAudioCapture>> retiredAudioCaptures_;
    std::vector<std::shared_ptr<FrameProcessor>> retiredFrameProcessors_;
    std::vector<std::shared_ptr<media::AudioRenderer>> retiredAudioRenderers_;

    TS3Functions ts3_ = {};
    std::mutex talkMutex_;
    std::unordered_set<anyID> talkingClients_;
};

} // namespace ts3stream::ui
