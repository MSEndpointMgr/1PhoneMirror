#pragma once

#include <openmirror/airplay/mdns_service.h>
#include <openmirror/airplay/mirror_buffer.h>
#include <openmirror/airplay/pairing.h>
#include <openmirror/airplay/srp_pin.h>
#include <openmirror/media/decoder.h>
#include <openmirror/network/rtsp_server.h>
#include <openmirror/network/tcp_server.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openmirror::airplay {

// AirPlay receiver server
// Handles the full AirPlay mirroring session:
// 1. mDNS advertisement (so iOS devices can discover us)
// 2. RTSP control channel (session setup, teardown)
// 3. Mirroring data channel (receives H.264 video stream)
// 4. Audio stream (receives AAC/ALAC audio)

class AirPlayServer {
public:
    AirPlayServer();
    ~AirPlayServer();

    struct Config {
        std::string server_name = "1PhoneMirror by MSEndpointMgr";
        uint16_t port = 7000;
        uint16_t mirror_port = 7100;
    };

    bool start(const Config& config);
    void stop();

    // Set callback for decoded video frames
    void set_video_callback(media::OnVideoFrame cb);
    void set_audio_callback(media::OnAudioFrame cb);

    using OnDisconnect = std::function<void()>;
    void set_disconnect_callback(OnDisconnect cb) { on_disconnect_ = std::move(cb); }

    // Called whenever the on-screen PIN should be shown or cleared.
    // The callback receives the 4-digit PIN string, or "" when pairing
    // completes (or fails) and the PIN should be hidden.
    using OnPinDisplay = std::function<void(const std::string&)>;
    void set_pin_display_callback(OnPinDisplay cb) { on_pin_display_ = std::move(cb); }

    // Enable PIN-required pairing for managed devices (advertises pw=true,
    // flags=0x44 in mDNS, and serves the /pair-pin-start endpoint).
    void set_require_pin(bool require) { require_pin_ = require; }
    bool require_pin() const { return require_pin_; }

    // ---- Multi-source support ----
    // A connected mirror source (one per iOS device currently streaming).
    struct SourceInfo {
        std::string id;     // stable id (client IP)
        std::string name;   // friendly display name (e.g. "Device 1")
        bool active = false;
        bool streaming = false; // mirror data socket currently open
        bool paused = false;    // client signaled video stream pause
    };

    using OnSourcesChanged = std::function<void(const std::vector<SourceInfo>&)>;
    void set_sources_callback(OnSourcesChanged cb) { on_sources_changed_ = std::move(cb); }

    std::vector<SourceInfo> list_sources();
    std::string active_source_id();
    void set_active_source(const std::string& id);
    // Forcefully drop a source's mirror connection. The picker dot disappears
    // and iOS may reconnect via a new SETUP if the user re-mirrors.
    void disconnect_source(const std::string& id);

private:
    // RTSP method handlers (the AirPlay control protocol)
    network::RtspResponse handle_info(const network::RtspRequest& req);
    network::RtspResponse handle_pair_setup(const network::RtspRequest& req);
    network::RtspResponse handle_pair_verify(const network::RtspRequest& req);
    network::RtspResponse handle_pair_pin_start(const network::RtspRequest& req);
    network::RtspResponse handle_pair_setup_pin(const network::RtspRequest& req);
    network::RtspResponse handle_fp_setup(const network::RtspRequest& req);
    network::RtspResponse handle_setup(const network::RtspRequest& req);
    network::RtspResponse handle_get_parameter(const network::RtspRequest& req);
    network::RtspResponse handle_set_parameter(const network::RtspRequest& req);
    network::RtspResponse handle_teardown(const network::RtspRequest& req);
    network::RtspResponse handle_default(const network::RtspRequest& req);

    // Mirror data receiver (runs on mirror_port)
    void mirror_receive_loop();

    // Event channel — separate TCP listener for reverse events
    bool start_event_listener();
    void event_accept_loop();

    // Timing channel — UDP socket for NTP sync
    bool start_timing_listener();
    void timing_loop();

    MdnsService mdns_;
    network::RtspServer rtsp_;
    media::Decoder decoder_;

    Pairing pairing_;
    FairPlay fairplay_;
    uint8_t hw_addr_[6] = {};

    // ---- Per-source state (registry keyed by client IP) ----
    struct MirrorSource {
        std::string id;       // client IP
        std::string name;     // "Device N"
        int number = 0;
        uint8_t aes_key[16] = {};
        bool has_aes_key = false;
        uint64_t stream_connection_id = 0;
        std::unique_ptr<MirrorBuffer> buffer;
        socket_t mirror_sock = INVALID_SOCK;
        // Set true when the iOS client signals video stream pause via the
        // SPS/PPS packet flag (0x56 / 0x5e). Cleared as soon as fresh video
        // data resumes. Surfaced to the UI via SourceInfo::paused.
        bool paused = false;
        // Per-source video decoder. Every connected source decodes
        // continuously into its own decoder; the active source's frames are
        // forwarded to the renderer. This makes switching instantaneous
        // (no resync wait, no socket kick) at the cost of CPU.
        std::unique_ptr<media::Decoder> video_decoder;
    };

    std::mutex sources_mutex_;
    std::map<std::string, std::unique_ptr<MirrorSource>> sources_;
    std::string active_source_id_;
    int next_source_number_ = 1;
    OnSourcesChanged on_sources_changed_;

    static std::string ip_from_addr(const std::string& addr);
    MirrorSource* get_or_create_source_locked(const std::string& ip);
    std::vector<SourceInfo> snapshot_sources_locked() const;
    void notify_sources_changed_locked();

    Config config_;

    // PIN pairing (SRP-6a) state
    SrpPinServer srp_pin_;
    std::mutex pin_mutex_;
    std::string current_pin_;
    bool require_pin_ = false;
    bool srp_active_ = false;
    OnPinDisplay on_pin_display_;

    std::atomic<bool> running_{false};
    std::thread mirror_thread_;

    // Event listener
    socket_t event_sock_ = INVALID_SOCK;
    uint16_t event_port_ = 0;
    std::thread event_thread_;

    // Timing (NTP) listener
    socket_t timing_sock_ = INVALID_SOCK;
    uint16_t timing_port_ = 0;
    std::thread timing_thread_;

    media::OnVideoFrame on_video_;
    media::OnAudioFrame on_audio_;
    OnDisconnect on_disconnect_;
};

} // namespace openmirror::airplay
