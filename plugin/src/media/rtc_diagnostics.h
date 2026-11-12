#pragma once

#include <livekit/stats.h>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ts3stream::media {

// Counter deltas, not lifetime averages. No addresses, tokens or media data.
// The caller supplies its monotonic time: SDK timestamp units vary by backend.
class RtcDiagnostics {
    struct Sample {
        double time = 0, workSeconds = 0, bufferSeconds = 0;
        uint64_t bytes = 0, frames = 0, packets = 0, retransmits = 0, bufferFrames = 0;
        uint64_t dropped = 0, freezes = 0, keyframes = 0, nack = 0, pli = 0;
        int64_t lost = 0;
        uint64_t concealed = 0, inserted = 0, removed = 0;
        double targetSeconds = 0;
    };
    std::map<std::string, Sample> previous_;
    template<class T> static T delta(T now, T old) { return now >= old ? now - old : T{}; }
public:
    std::vector<std::string> update(const livekit::SessionStats& stats, double nowSeconds) {
        std::vector<std::string> lines;
        std::map<std::string, Sample> current;
        auto collect = [&](const std::vector<livekit::RtcStats>& list) {
            for (const auto& record : list) {
                Sample s;
                s.time = nowSeconds;
                std::string id, implementation;
                bool outbound = false;
                bool audio = false;
                unsigned width = 0, height = 0;
                double target = 0;
                int limitation = -1;
                if (const auto* out = std::get_if<livekit::RtcOutboundRtpStats>(&record.stats)) {
                    if (out->stream.kind != "video") continue;
                    outbound = true;
                    id = "tx/" + out->rtc.id;
                    const auto& v = out->outbound;
                    s.bytes = out->sent.bytes_sent; s.packets = out->sent.packets_sent;
                    s.frames = v.frames_encoded; s.workSeconds = v.total_encode_time;
                    s.bufferSeconds = v.total_packet_send_delay; s.bufferFrames = s.packets;
                    s.retransmits = v.retransmitted_packets_sent; s.keyframes = v.key_frames_encoded;
                    s.nack = v.nack_count; s.pli = v.pli_count;
                    width = v.frame_width; height = v.frame_height;
                    target = v.target_bitrate / 1e6;
                    limitation = static_cast<int>(v.quality_limitation_reason);
                    implementation = v.encoder_implementation;
                } else if (const auto* in = std::get_if<livekit::RtcInboundRtpStats>(&record.stats)) {
                    audio = in->stream.kind == "audio";
                    if (!audio && in->stream.kind != "video") continue;
                    id = std::string(audio ? "audio_rx/" : "rx/") + in->rtc.id;
                    const auto& v = in->inbound;
                    s.bytes = v.bytes_received; s.packets = in->received.packets_received;
                    s.lost = in->received.packets_lost; s.frames = v.frames_decoded;
                    s.workSeconds = v.total_decode_time;
                    s.bufferSeconds = v.jitter_buffer_delay; s.bufferFrames = v.jitter_buffer_emitted_count;
                    s.dropped = v.frames_dropped; s.freezes = v.freeze_count;
                    s.keyframes = v.key_frames_decoded; s.nack = v.nack_count; s.pli = v.pli_count;
                    s.retransmits = v.retransmitted_packets_received;
                    width = v.frame_width; height = v.frame_height;
                    implementation = v.decoder_implementation;
                    if (audio) {
                        s.frames = v.total_samples_received;
                        s.concealed = v.concealed_samples;
                        s.inserted = v.inserted_samples_for_deceleration;
                        s.removed = v.removed_samples_for_acceleration;
                        s.targetSeconds = v.jitter_buffer_target_delay;
                    }
                } else {
                    continue;
                }
                // Single-PC rooms can report the same record in both lists.
                if (!current.emplace(id, s).second) continue;
                auto old = previous_.find(id);
                if (old == previous_.end() || nowSeconds <= old->second.time ||
                    s.bytes < old->second.bytes || s.frames < old->second.frames) continue;
                const auto& p = old->second;
                const double dt = nowSeconds - p.time;
                const auto frames = delta(s.frames, p.frames);
                const auto bufferFrames = delta(s.bufferFrames, p.bufferFrames);
                char line[1400]{};
                if (audio) {
                    std::snprintf(line, sizeof(line),
                        "rtc audio_rx id=%s kbps=%.1f samples_s=%.1f jitter_buffer_ms=%.2f target_ms=%.2f "
                        "lost=%lld concealed=%llu inserted=%llu removed=%llu",
                        id.c_str(), delta(s.bytes, p.bytes) * 8.0 / dt / 1000, frames / dt,
                        bufferFrames ? delta(s.bufferSeconds, p.bufferSeconds) * 1000 / bufferFrames : 0,
                        bufferFrames ? delta(s.targetSeconds, p.targetSeconds) * 1000 / bufferFrames : 0,
                        static_cast<long long>(delta(s.lost, p.lost)),
                        static_cast<unsigned long long>(delta(s.concealed, p.concealed)),
                        static_cast<unsigned long long>(delta(s.inserted, p.inserted)),
                        static_cast<unsigned long long>(delta(s.removed, p.removed)));
                    lines.emplace_back(line);
                    continue;
                }
                std::snprintf(line, sizeof(line),
                    "rtc %s id=%s mbps=%.3f fps=%.1f size=%ux%u work_ms=%.2f %s_ms=%.2f "
                    "lost=%lld rtx=%llu dropped=%llu freezes=%llu keyframes=%llu nack=%llu pli=%llu "
                    "target_mbps=%.3f limitation=%d implementation=%s",
                    outbound ? "tx" : "rx", id.c_str(), delta(s.bytes, p.bytes) * 8.0 / dt / 1e6,
                    frames / dt, width, height,
                    frames ? delta(s.workSeconds, p.workSeconds) * 1000 / frames : 0,
                    outbound ? "send_queue" : "jitter_buffer",
                    bufferFrames ? delta(s.bufferSeconds, p.bufferSeconds) * 1000 / bufferFrames : 0,
                    static_cast<long long>(delta(s.lost, p.lost)),
                    static_cast<unsigned long long>(delta(s.retransmits, p.retransmits)),
                    static_cast<unsigned long long>(delta(s.dropped, p.dropped)),
                    static_cast<unsigned long long>(delta(s.freezes, p.freezes)),
                    static_cast<unsigned long long>(delta(s.keyframes, p.keyframes)),
                    static_cast<unsigned long long>(delta(s.nack, p.nack)),
                    static_cast<unsigned long long>(delta(s.pli, p.pli)), target, limitation, implementation.c_str());
                lines.emplace_back(line);
            }
        };
        collect(stats.publisher_stats);
        collect(stats.subscriber_stats);
        previous_ = std::move(current);
        return lines;
    }
};

} // namespace ts3stream::media
