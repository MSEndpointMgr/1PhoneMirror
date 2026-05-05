#pragma once

#include <openmirror/airplay/mdns_service.h>
#include <openmirror/airplay/mirror_buffer.h>
#include <openmirror/airplay/pairing.h>
#include <openmirror/media/decoder.h>
#include <openmirror/network/rtsp_server.h>
#include <openmirror/network/tcp_server.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

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

private:
    // RTSP method handlers (the AirPlay control protocol)
    network::RtspResponse handle_info(const network::RtspRequest& req);
    network::RtspResponse handle_pair_setup(const network::RtspRequest& req);
    network::RtspResponse handle_pair_verify(const network::RtspRequest& req);
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
    MirrorBuffer mirror_buffer_;

    Config config_;
    uint8_t hw_addr_[6] = {};
    uint8_t aes_key_[16] = {};       // FairPlay-decrypted AES key
    bool has_aes_key_ = false;
    uint64_t stream_connection_id_ = 0;

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
