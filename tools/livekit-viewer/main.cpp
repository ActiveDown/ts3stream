#include "livekit/livekit.h"

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QMetaObject>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

class VideoWidget : public QWidget {
public:
    explicit VideoWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(640, 360);
        setStyleSheet("background: #101010;");
    }

    void setFrame(const QImage& frame) {
        frame_ = frame;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (frame_.isNull()) {
            painter.setPen(Qt::white);
            painter.drawText(rect(), Qt::AlignCenter, tr("Ожидание видео…"));
            return;
        }
        const QImage scaled = frame_.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        painter.drawImage((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled);
    }

private:
    QImage frame_;
};

class ViewerDelegate : public livekit::RoomDelegate {
public:
    ViewerDelegate(livekit::Room& room, VideoWidget& widget) : room_(room), widget_(widget) {}

    void onTrackSubscribed(livekit::Room&, const livekit::TrackSubscribedEvent& event) override {
        if (!event.track || !event.participant || !event.publication) {
            return;
        }
        std::printf("track subscribed: participant=%s track=%s kind=%d\n",
                    event.participant->identity().c_str(),
                    event.publication->name().c_str(),
                    static_cast<int>(event.track->kind()));
        std::fflush(stdout);

        if (event.track->kind() != livekit::TrackKind::KIND_VIDEO) {
            return;
        }

        livekit::VideoStream::Options options;
        options.capacity = 2;
        options.format = livekit::VideoBufferType::RGBA;

        const std::string identity = event.participant->identity();
        const std::string trackName = event.publication->name();
        room_.setOnVideoFrameCallback(identity, trackName,
            [widget = &widget_](const livekit::VideoFrame& frame, int64_t timestampUs) {
                if (frame.width() <= 0 || frame.height() <= 0) {
                    return;
                }

                QImage image(frame.width(), frame.height(), QImage::Format_RGBA8888);
                const int rowBytes = frame.width() * 4;
                for (int y = 0; y < frame.height(); ++y) {
                    std::memcpy(image.scanLine(y),
                                frame.data() + static_cast<size_t>(y) * rowBytes,
                                rowBytes);
                }

                QMetaObject::invokeMethod(qApp, [widget, image, timestampUs] {
                    widget->setFrame(image);
                }, Qt::QueuedConnection);
            },
            options);
    }

    void onParticipantConnected(livekit::Room&, const livekit::ParticipantConnectedEvent& event) override {
        if (event.participant) {
            std::printf("participant connected: %s\n", event.participant->identity().c_str());
            std::fflush(stdout);
        }
    }

private:
    livekit::Room& room_;
    VideoWidget& widget_;
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    if (argc < 3) {
        std::printf("usage: livekit-viewer <ws-url> <token>\n");
        return 1;
    }

    const std::string url = argv[1];
    const std::string token = argv[2];

    auto* window = new QWidget;
    window->setWindowTitle("TS3Stream LiveKit Viewer");
    window->resize(1100, 700);
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* video = new VideoWidget(window);
    layout->addWidget(video);
    window->show();

    auto* room = new livekit::Room;
    auto* delegate = new ViewerDelegate(*room, *video);
    room->setDelegate(delegate);

    const bool initialized = livekit::initialize(livekit::LogLevel::Info);
    std::printf("LiveKit initialized=%d\n", initialized ? 1 : 0);

    QTimer::singleShot(100, [&app, room, url, token, video] {
        livekit::RoomOptions options;
        options.auto_subscribe = true;
        options.adaptive_stream = false;
        options.dynacast = false;

        std::printf("connecting to %s\n", url.c_str());
        std::fflush(stdout);

        if (!room->connect(url, token, options)) {
            std::printf("connect failed\n");
            video->setFrame(QImage());
            return;
        }

        std::printf("connected\n");
        std::fflush(stdout);
    });

    const int result = app.exec();
    delete delegate;
    delete room;
    return result;
}
