#pragma once

#ifdef ENABLE_CAST

#include <opm/media/decoder.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace opm::cast {

// Google Cast v2 receiver — lets Android devices discover and cast their screen.
//
// Protocol overview:
//   1. mDNS _googlecast._tcp advertisement (Bonjour or multicast)
//   2. TLS server on port 8009 — CastChannel protobuf messages
//   3. Session management: heartbeat, receiver status, app launch
//   4. Screen mirroring: WebRTC offer/answer → DTLS-SRTP → H.264 RTP
//
// Phase 1: Cast protocol (discovery + TLS + CastChannel + mirroring launch)
// Phase 2: WebRTC media reception (DTLS-SRTP + RTP depacketization)

class CastReceiver {
public:
    CastReceiver();
    ~CastReceiver();

    struct Config {
        std::string device_name = "1PhoneMirror";
        uint16_t port = 8009;
    };

    bool start(const Config& config);
    void stop();
    bool is_running() const { return running_.load(); }

    void set_video_callback(media::OnVideoFrame cb) { on_video_ = std::move(cb); }
    void set_audio_callback(media::OnAudioFrame cb) { on_audio_ = std::move(cb); }
    void set_disconnect_callback(std::function<void()> cb) { on_disconnect_ = std::move(cb); }

private:
    struct Impl;
    Impl* impl_ = nullptr;

    std::atomic<bool> running_{false};

    media::OnVideoFrame on_video_;
    media::OnAudioFrame on_audio_;
    std::function<void()> on_disconnect_;
};

} // namespace opm::cast

#endif // ENABLE_CAST
