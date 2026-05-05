#pragma once

#ifdef ENABLE_MIRACAST

#include <openmirror/media/decoder.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace openmirror::miracast {

// Miracast receiver using the Windows.Media.Miracast WinRT API
// This is the simplest way to receive Miracast (Wi-Fi Display) on Windows.
//
// Android devices use Wi-Fi Direct + Miracast for native screen casting.
// The WinRT MiracastReceiver API handles:
// - Wi-Fi Direct P2P discovery and connection
// - WFD capability negotiation (RTSP)
// - MPEG2-TS + H.264 stream reception
// - HDCP (optional)

class MiracastReceiver {
public:
    MiracastReceiver();
    ~MiracastReceiver();

    struct Config {
        std::string display_name = "1PhoneMirror";
        bool require_pin = false;
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
    Impl* impl_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread sta_thread_;
    unsigned long sta_thread_id_ = 0;

    media::OnVideoFrame on_video_;
    media::OnAudioFrame on_audio_;
    std::function<void()> on_disconnect_;
};

} // namespace openmirror::miracast

#endif // ENABLE_MIRACAST
