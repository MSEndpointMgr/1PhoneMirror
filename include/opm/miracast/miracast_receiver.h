#pragma once

#ifdef ENABLE_MIRACAST

#include <opm/media/decoder.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace opm::miracast {

// Miracast receiver using WiFi Direct + WFD (Wi-Fi Display) protocol.
//
// Minimal OS requirements:
// - Windows 10 (any version) with a WiFi Direct capable adapter
// - Does NOT require "Projecting to this PC" to be enabled
// - Does NOT require the "Wireless Display" optional feature
//
// Implementation:
// - WiFiDirectAdvertisementPublisher advertises as a WFD sink
// - WiFiDirectConnectionListener accepts P2P connections
// - WFD RTSP session (M1-M7) negotiates capabilities
// - MPEG2-TS stream received over RTP/TCP
// - FFmpeg decodes H.264 video and AAC/LPCM audio

class MiracastReceiver {
public:
    MiracastReceiver();
    ~MiracastReceiver();

    struct Config {
        std::string display_name = "1PhoneMirror";
        bool require_pin = false;
        uint16_t rtsp_port = 7236;  // Standard WFD RTSP port
    };

    bool start(const Config& config);
    void stop();

    bool is_running() const { return running_.load(); }

    // Set callback for decoded video frames
    void set_video_callback(media::OnVideoFrame cb) { on_video_ = std::move(cb); }
    void set_audio_callback(media::OnAudioFrame cb) { on_audio_ = std::move(cb); }
    void set_disconnect_callback(std::function<void()> cb) { on_disconnect_ = std::move(cb); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> running_{false};
    std::thread worker_thread_;

    media::OnVideoFrame on_video_;
    media::OnAudioFrame on_audio_;
    std::function<void()> on_disconnect_;
};

} // namespace opm::miracast

#endif // ENABLE_MIRACAST
