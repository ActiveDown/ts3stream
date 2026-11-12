#include "media/rtc_diagnostics.h"
#include <cstdio>

int main() {
    using namespace livekit;
    ts3stream::media::RtcDiagnostics diagnostics;
    RtcOutboundRtpStats tx{};
    tx.rtc.id = "video-out"; tx.stream.kind = "video";
    tx.sent.bytes_sent = 1000; tx.sent.packets_sent = 10;
    tx.outbound.frames_encoded = 10; tx.outbound.total_encode_time = 0.1;
    tx.outbound.total_packet_send_delay = 0.01;
    RtcInboundRtpStats rx{};
    rx.rtc.id = "video-in"; rx.stream.kind = "video";
    rx.inbound.bytes_received = 1000; rx.inbound.frames_decoded = 10;
    rx.inbound.total_decode_time = 0.02;
    rx.inbound.jitter_buffer_emitted_count = 10; rx.inbound.jitter_buffer_delay = 1;
    auto stats = [&] {
        SessionStats s;
        s.publisher_stats = {RtcStats{tx}, RtcStats{rx}};
        s.subscriber_stats = s.publisher_stats; // Single-PC duplicate records.
        return s;
    };
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL: %s\n", what); } };
    check(diagnostics.update(stats(), 1).empty(), "first report establishes baseline");
    tx.sent.bytes_sent += 2500000; tx.sent.packets_sent += 2000;
    tx.outbound.frames_encoded += 60; tx.outbound.total_encode_time += 0.3;
    tx.outbound.total_packet_send_delay += 4;
    tx.outbound.target_bitrate = 10000000;
    rx.inbound.bytes_received += 2000000; rx.inbound.frames_decoded += 50;
    rx.inbound.total_decode_time += 0.1;
    rx.inbound.jitter_buffer_emitted_count += 50; rx.inbound.jitter_buffer_delay += 100;
    rx.received.packets_lost = 3; rx.inbound.freeze_count = 1;
    auto lines = diagnostics.update(stats(), 3);
    check(lines.size() == 2, "duplicate records are not logged twice");
    if (lines.size() == 2) {
        check(lines[0].find("mbps=10.000 fps=30.0") != std::string::npos, "transmit rate from interval byte/frame deltas");
        check(lines[0].find("work_ms=5.00 send_queue_ms=2.00") != std::string::npos, "encode and send wait per interval");
        check(lines[1].find("mbps=8.000 fps=25.0") != std::string::npos, "receive interval rates");
        check(lines[1].find("jitter_buffer_ms=2000.00 lost=3") != std::string::npos, "receive jitter delay and loss deltas");
        check(lines[1].find("freezes=1") != std::string::npos, "freeze counter delta");
    }
    tx.sent.bytes_sent = 0; tx.outbound.frames_encoded = 0;
    rx.inbound.bytes_received = 0; rx.inbound.frames_decoded = 0;
    check(diagnostics.update(stats(), 5).empty(), "counter reset starts a new baseline instead of underflow");
    lines = diagnostics.update(stats(), 7);
    check(lines.size() == 2 && lines[0].find("mbps=0.000 fps=0.0") != std::string::npos,
          "idle reports zero rates without division by zero");
    check(diagnostics.update({}, 9).empty(), "removed tracks are forgotten");
    check(diagnostics.update(stats(), 11).empty(), "reappearing track gets fresh baseline");
    RtcInboundRtpStats audio{};
    audio.rtc.id = "audio-in"; audio.stream.kind = "audio";
    SessionStats audioStats;
    audioStats.subscriber_stats = {RtcStats{audio}};
    check(diagnostics.update(audioStats, 13).empty(), "audio establishes its own baseline");
    audio.inbound.bytes_received = 8000;
    audio.inbound.total_samples_received = 96000;
    audio.inbound.jitter_buffer_emitted_count = 96000;
    audio.inbound.jitter_buffer_delay = 96000 * 0.4;
    audio.inbound.jitter_buffer_target_delay = 96000 * 2.0;
    audio.inbound.inserted_samples_for_deceleration = 1920;
    audio.inbound.removed_samples_for_acceleration = 480;
    audio.inbound.concealed_samples = 960;
    audioStats.subscriber_stats = {RtcStats{audio}};
    lines = diagnostics.update(audioStats, 15);
    check(lines.size() == 1 && lines[0].find("samples_s=48000.0 jitter_buffer_ms=400.00 target_ms=2000.00") != std::string::npos &&
          lines[0].find("concealed=960 inserted=1920 removed=480") != std::string::npos,
          "audio buffer growth and time stretching measured separately from video");
    std::printf("RTC diagnostics counter deltas: failures=%d\n", failures);
    return failures ? 1 : 0;
}
