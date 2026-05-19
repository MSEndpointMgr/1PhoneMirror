// =============================================================================
// Miracast Receiver — WiFi Direct + WFD Protocol Implementation
//
// Minimal OS requirements:
// - Windows 10 (any version) with WiFi Direct capable adapter
// - Does NOT require "Projecting to this PC" system setting
// - Does NOT require "Wireless Display" optional feature
//
// Protocol layers:
// 1. WiFi Direct P2P advertisement (WFD sink IE)
// 2. WFD RTSP session negotiation (M1–M7)
// 3. MPEG2-TS over RTP (UDP)
// 4. H.264/AAC decode via FFmpeg
// =============================================================================

#ifdef ENABLE_MIRACAST

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <opm/miracast/miracast_receiver.h>
#include <opm/network/tcp_server.h>
#include <iostream>
#include <string>
#include <sstream>
#include <mutex>
#include <vector>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <future>

// WinRT headers for WiFi Direct (minimal subset — no Miracast-specific APIs)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.WiFiDirect.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Security.Credentials.h>

// FFmpeg for MPEG2-TS demuxing and H.264/AAC decoding
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace WFDir = winrt::Windows::Devices::WiFiDirect;
namespace WNS   = winrt::Windows::Networking::Sockets;
namespace WSS   = winrt::Windows::Storage::Streams;
namespace WF    = winrt::Windows::Foundation;

namespace opm::miracast {

using opm::network::TcpServer;

// ============================================================================
// WFD Capability Strings
// ============================================================================

// Basic H.264 CBP L3.1 support (CEA 720p30 + 1080p30)
static const char* WFD_VIDEO_FORMATS =
    "00 00 02 02 00000001 00000000 00000000 00 0000 0000 00 none none";

// LPCM 48kHz stereo
static const char* WFD_AUDIO_CODECS = "LPCM 00000002 00";

// ============================================================================
// WFD RTSP State
// ============================================================================

enum class WfdState {
    Idle,
    M1_Received,      // Source sent OPTIONS to us
    M2_Sent,          // We sent OPTIONS to source
    M3_Received,      // Source asked for capabilities (GET_PARAMETER)
    M4_Received,      // Source set parameters (SET_PARAMETER)
    M5_Received,      // Source triggered SETUP
    M6_Sent,          // We sent SETUP
    M7_Sent,          // We sent PLAY
    Streaming,
    Teardown
};

// ============================================================================
// MPEG2-TS Demuxer using FFmpeg (custom I/O from ring buffer)
// ============================================================================

struct TsDemuxer {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* video_ctx = nullptr;
    AVCodecContext* audio_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;

    AVIOContext* avio_ctx = nullptr;
    uint8_t* avio_buffer = nullptr;
    static constexpr int AVIO_BUF_SIZE = 32768;

    // Incoming TS data buffer (fed from RTP thread)
    std::mutex buf_mutex;
    std::vector<uint8_t> ts_buffer;
    size_t ts_read_pos = 0;
    std::atomic<bool> eof{false};
    std::atomic<bool> has_data{false};

    bool init() {
        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        return frame && pkt;
    }

    void feed(const uint8_t* data, int len) {
        std::lock_guard<std::mutex> lock(buf_mutex);
        ts_buffer.insert(ts_buffer.end(), data, data + len);
        has_data.store(true);
    }

    void signal_eof() { eof.store(true); }

    static int avio_read_cb(void* opaque, uint8_t* buf, int buf_size) {
        auto* self = static_cast<TsDemuxer*>(opaque);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(self->buf_mutex);
                size_t avail = self->ts_buffer.size() - self->ts_read_pos;
                if (avail > 0) {
                    int n = std::min(static_cast<int>(avail), buf_size);
                    memcpy(buf, self->ts_buffer.data() + self->ts_read_pos, n);
                    self->ts_read_pos += n;
                    if (self->ts_read_pos > 512 * 1024) {
                        self->ts_buffer.erase(
                            self->ts_buffer.begin(),
                            self->ts_buffer.begin() + self->ts_read_pos);
                        self->ts_read_pos = 0;
                    }
                    return n;
                }
                if (self->eof.load()) return AVERROR_EOF;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool open_stream() {
        avio_buffer = static_cast<uint8_t*>(av_malloc(AVIO_BUF_SIZE));
        if (!avio_buffer) return false;

        avio_ctx = avio_alloc_context(
            avio_buffer, AVIO_BUF_SIZE, 0, this, avio_read_cb, nullptr, nullptr);
        if (!avio_ctx) { av_free(avio_buffer); return false; }

        fmt_ctx = avformat_alloc_context();
        if (!fmt_ctx) return false;
        fmt_ctx->pb = avio_ctx;

        const AVInputFormat* ifmt = av_find_input_format("mpegts");
        if (avformat_open_input(&fmt_ctx, nullptr, ifmt, nullptr) < 0) {
            std::cerr << "[Miracast] Failed to open MPEG-TS stream\n";
            return false;
        }

        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
            std::cerr << "[Miracast] No stream info in MPEG-TS\n";
            return false;
        }

        for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
            auto type = fmt_ctx->streams[i]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO && video_stream_idx < 0)
                video_stream_idx = static_cast<int>(i);
            else if (type == AVMEDIA_TYPE_AUDIO && audio_stream_idx < 0)
                audio_stream_idx = static_cast<int>(i);
        }

        if (video_stream_idx < 0) {
            std::cerr << "[Miracast] No video in MPEG-TS\n";
            return false;
        }

        // Video decoder
        auto* vcodec = avcodec_find_decoder(
            fmt_ctx->streams[video_stream_idx]->codecpar->codec_id);
        if (!vcodec) return false;
        video_ctx = avcodec_alloc_context3(vcodec);
        avcodec_parameters_to_context(video_ctx,
            fmt_ctx->streams[video_stream_idx]->codecpar);
        video_ctx->thread_count = 2;
        if (avcodec_open2(video_ctx, vcodec, nullptr) < 0) return false;

        // Audio decoder
        if (audio_stream_idx >= 0) {
            auto* acodec = avcodec_find_decoder(
                fmt_ctx->streams[audio_stream_idx]->codecpar->codec_id);
            if (acodec) {
                audio_ctx = avcodec_alloc_context3(acodec);
                avcodec_parameters_to_context(audio_ctx,
                    fmt_ctx->streams[audio_stream_idx]->codecpar);
                if (avcodec_open2(audio_ctx, acodec, nullptr) < 0) {
                    avcodec_free_context(&audio_ctx);
                    audio_ctx = nullptr;
                }
            }
        }

        std::cout << "[Miracast] Stream opened: video="
                  << avcodec_get_name(video_ctx->codec_id);
        if (audio_ctx)
            std::cout << " audio=" << avcodec_get_name(audio_ctx->codec_id);
        std::cout << "\n";
        return true;
    }

    bool decode_next(media::OnVideoFrame& on_video, media::OnAudioFrame& on_audio) {
        if (av_read_frame(fmt_ctx, pkt) < 0) return false;

        if (pkt->stream_index == video_stream_idx) {
            if (avcodec_send_packet(video_ctx, pkt) == 0) {
                while (avcodec_receive_frame(video_ctx, frame) == 0)
                    emit_video(on_video);
            }
        } else if (pkt->stream_index == audio_stream_idx && audio_ctx) {
            if (avcodec_send_packet(audio_ctx, pkt) == 0) {
                while (avcodec_receive_frame(audio_ctx, frame) == 0)
                    emit_audio(on_audio);
            }
        }
        av_packet_unref(pkt);
        return true;
    }

    void emit_video(media::OnVideoFrame& cb) {
        if (!cb) return;
        int w = frame->width, h = frame->height;
        sws_ctx = sws_getCachedContext(sws_ctx,
            w, h, static_cast<AVPixelFormat>(frame->format),
            w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx) return;

        media::VideoFrame vf;
        vf.width = w;
        vf.height = h;
        vf.stride = w * 4;
        vf.data = new uint8_t[vf.stride * h];

        uint8_t* dst[1] = { vf.data };
        int dst_stride[1] = { vf.stride };
        sws_scale(sws_ctx, frame->data, frame->linesize, 0, h, dst, dst_stride);
        cb(std::move(vf));
    }

    void emit_audio(media::OnAudioFrame& cb) {
        if (!cb || !audio_ctx) return;

        if (!swr_ctx) {
            AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
            swr_alloc_set_opts2(&swr_ctx,
                &out_layout, AV_SAMPLE_FMT_S16, 48000,
                &audio_ctx->ch_layout, audio_ctx->sample_fmt,
                audio_ctx->sample_rate, 0, nullptr);
            if (!swr_ctx || swr_init(swr_ctx) < 0) {
                swr_free(&swr_ctx);
                return;
            }
        }

        int out_samples = swr_get_out_samples(swr_ctx, frame->nb_samples);
        if (out_samples <= 0) return;

        int out_size = out_samples * 2 * 2; // stereo * 16-bit
        media::AudioFrame af;
        af.data = std::make_unique<uint8_t[]>(out_size);
        af.size = out_size;
        af.sample_rate = 48000;
        af.channels = 2;

        uint8_t* out_buf[1] = { af.data.get() };
        int converted = swr_convert(swr_ctx, out_buf, out_samples,
            const_cast<const uint8_t**>(frame->data), frame->nb_samples);
        af.size = converted * 2 * 2;
        cb(std::move(af));
    }

    void cleanup() {
        if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
        if (swr_ctx) { swr_free(&swr_ctx); swr_ctx = nullptr; }
        if (video_ctx) avcodec_free_context(&video_ctx);
        if (audio_ctx) avcodec_free_context(&audio_ctx);
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        if (avio_ctx) {
            // avio_buffer freed by avformat_close_input or avio_context_free
            avio_context_free(&avio_ctx);
        }
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
    }
};

// ============================================================================
// WFD RTSP Session
// ============================================================================

struct WfdSession {
    socket_t sock = INVALID_SOCK;
    WfdState state = WfdState::Idle;
    int cseq_out = 0;       // Our outgoing CSeq counter
    uint16_t rtp_port = 0;
    std::string session_id;
    std::string presentation_url;

    bool read_line(std::string& out) {
        out = TcpServer::recv_line(sock);
        return !out.empty() || true; // empty line is valid (header terminator)
    }

    struct Message {
        std::string method;  // For requests
        std::string uri;
        int status = 0;      // For responses
        std::map<std::string, std::string> headers;
        std::string body;
        int cseq = 0;
        bool is_request = false;
    };

    Message read_message() {
        Message msg;
        std::string first_line = TcpServer::recv_line(sock);
        if (first_line.empty() && GetLastError() != 0) return msg; // Disconnected

        if (first_line.substr(0, 4) == "RTSP") {
            // Response: "RTSP/1.0 200 OK"
            msg.is_request = false;
            std::istringstream iss(first_line);
            std::string ver;
            iss >> ver >> msg.status;
        } else {
            // Request: "METHOD uri RTSP/1.0"
            msg.is_request = true;
            std::istringstream iss(first_line);
            std::string ver;
            iss >> msg.method >> msg.uri >> ver;
        }

        // Read headers
        for (;;) {
            std::string line = TcpServer::recv_line(sock);
            if (line.empty()) break;
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon);
                std::string v = line.substr(colon + 1);
                while (!v.empty() && v[0] == ' ') v.erase(v.begin());
                msg.headers[k] = v;
            }
        }

        if (msg.headers.count("CSeq"))
            msg.cseq = std::stoi(msg.headers["CSeq"]);

        if (msg.headers.count("Content-Length")) {
            int len = std::stoi(msg.headers["Content-Length"]);
            if (len > 0 && len < 65536) {
                std::vector<uint8_t> buf(len);
                TcpServer::recv_exact(sock, buf.data(), len);
                msg.body.assign(buf.begin(), buf.end());
            }
        }

        return msg;
    }

    void send_response(int status_code, const std::string& reason, int resp_cseq,
                       const std::string& body = {},
                       const std::map<std::string, std::string>& hdrs = {}) {
        std::ostringstream oss;
        oss << "RTSP/1.0 " << status_code << " " << reason << "\r\n";
        oss << "CSeq: " << resp_cseq << "\r\n";
        for (auto& [k, v] : hdrs) oss << k << ": " << v << "\r\n";
        if (!body.empty()) {
            oss << "Content-Type: text/parameters\r\n";
            oss << "Content-Length: " << body.size() << "\r\n";
        }
        oss << "\r\n" << body;
        TcpServer::send_string(sock, oss.str());
    }

    void send_request(const std::string& method, const std::string& uri,
                      const std::string& body = {},
                      const std::map<std::string, std::string>& hdrs = {}) {
        cseq_out++;
        std::ostringstream oss;
        oss << method << " " << uri << " RTSP/1.0\r\n";
        oss << "CSeq: " << cseq_out << "\r\n";
        for (auto& [k, v] : hdrs) oss << k << ": " << v << "\r\n";
        if (!body.empty()) {
            oss << "Content-Type: text/parameters\r\n";
            oss << "Content-Length: " << body.size() << "\r\n";
        }
        oss << "\r\n" << body;
        TcpServer::send_string(sock, oss.str());
    }

    // Run the WFD handshake (M1 through M7)
    bool negotiate() {
        // Wait for M1: Source → Sink OPTIONS
        std::cout << "[Miracast] Waiting for WFD handshake...\n";

        for (;;) {
            auto msg = read_message();
            if (!msg.is_request && msg.method.empty() && msg.status == 0)
                return false; // Disconnect

            if (msg.is_request) {
                std::cout << "[Miracast] RTSP << " << msg.method
                          << " (CSeq=" << msg.cseq << ")\n";

                if (msg.method == "OPTIONS" && state == WfdState::Idle) {
                    // M1: respond with our supported methods
                    send_response(200, "OK", msg.cseq, {},
                        {{"Public", "org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER"}});
                    state = WfdState::M1_Received;

                    // M2: Send our OPTIONS to source
                    send_request("OPTIONS", "*", {},
                        {{"Require", "org.wfa.wfd1.0"}});
                    state = WfdState::M2_Sent;

                } else if (msg.method == "GET_PARAMETER") {
                    // M3: Source asks for our capabilities
                    std::ostringstream body;
                    body << "wfd_video_formats: " << WFD_VIDEO_FORMATS << "\r\n";
                    body << "wfd_audio_codecs: " << WFD_AUDIO_CODECS << "\r\n";
                    body << "wfd_content_protection: none\r\n";
                    body << "wfd_coupled_sink: none\r\n";
                    body << "wfd_client_rtp_ports: RTP/AVP/UDP;unicast "
                         << rtp_port << " 0 mode=play\r\n";
                    send_response(200, "OK", msg.cseq, body.str());
                    state = WfdState::M3_Received;

                } else if (msg.method == "SET_PARAMETER") {
                    if (msg.body.find("wfd_trigger_method: SETUP") != std::string::npos) {
                        // M5: Trigger — source wants us to send SETUP
                        send_response(200, "OK", msg.cseq);
                        state = WfdState::M5_Received;

                        // M6: Send SETUP
                        if (presentation_url.empty())
                            presentation_url = "rtsp://localhost/wfd1.0/streamid=0";

                        std::string transport = "RTP/AVP/UDP;unicast;client_port=" +
                                                std::to_string(rtp_port);
                        send_request("SETUP", presentation_url, {},
                            {{"Transport", transport}});
                        state = WfdState::M6_Sent;

                    } else {
                        // M4: Source sets session parameters
                        auto pos = msg.body.find("wfd_presentation_URL:");
                        if (pos != std::string::npos) {
                            auto start = pos + strlen("wfd_presentation_URL:");
                            while (start < msg.body.size() && msg.body[start] == ' ')
                                start++;
                            auto end = msg.body.find_first_of("\r\n", start);
                            std::string url_line = msg.body.substr(start,
                                end == std::string::npos ? std::string::npos : end - start);
                            auto sp = url_line.find(' ');
                            presentation_url = (sp != std::string::npos)
                                ? url_line.substr(0, sp) : url_line;
                        }
                        send_response(200, "OK", msg.cseq);
                        state = WfdState::M4_Received;
                    }

                } else if (msg.method == "TEARDOWN") {
                    send_response(200, "OK", msg.cseq);
                    state = WfdState::Teardown;
                    return false;

                } else {
                    send_response(200, "OK", msg.cseq);
                }
            } else {
                // Response to something we sent
                std::cout << "[Miracast] RTSP >> " << msg.status
                          << " (CSeq=" << msg.cseq << ")\n";

                if (state == WfdState::M2_Sent && msg.status == 200) {
                    // M2 acknowledged — wait for M3
                } else if (state == WfdState::M6_Sent && msg.status == 200) {
                    // SETUP accepted
                    if (msg.headers.count("Session")) {
                        session_id = msg.headers["Session"];
                        auto semi = session_id.find(';');
                        if (semi != std::string::npos)
                            session_id = session_id.substr(0, semi);
                    }

                    // M7: Send PLAY
                    send_request("PLAY", presentation_url, {},
                        {{"Session", session_id}});
                    state = WfdState::M7_Sent;

                } else if (state == WfdState::M7_Sent && msg.status == 200) {
                    // Streaming!
                    state = WfdState::Streaming;
                    std::cout << "[Miracast] WFD streaming started (RTP port "
                              << rtp_port << ")\n";
                    return true;
                }
            }
        }
    }

    // Keep-alive loop during streaming
    bool handle_message() {
        auto msg = read_message();
        if (!msg.is_request && msg.method.empty() && msg.status == 0)
            return false;

        if (msg.is_request) {
            if (msg.method == "TEARDOWN") {
                send_response(200, "OK", msg.cseq);
                return false;
            }
            send_response(200, "OK", msg.cseq);
        }
        return true;
    }
};

// ============================================================================
// Impl
// ============================================================================

struct MiracastReceiver::Impl {
    // WiFi Direct
    WFDir::WiFiDirectAdvertisementPublisher publisher{nullptr};
    WFDir::WiFiDirectConnectionListener conn_listener{nullptr};
    winrt::event_token pub_status_token;
    winrt::event_token conn_req_token;

    // Session
    WfdSession session;
    TsDemuxer demuxer;

    // RTP
    socket_t rtp_sock = INVALID_SOCK;
    uint16_t rtp_port = 0;
    std::thread rtp_thread;
    std::thread decode_thread;
    std::thread rtsp_thread;

    // RTSP accept
    socket_t rtsp_listen_sock = INVALID_SOCK;
    uint16_t rtsp_port = 7236;

    std::atomic<bool> streaming{false};
    std::atomic<bool> session_active{false};

    DWORD worker_thread_id = 0;

    bool setup_rtp_socket() {
        rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (rtp_sock == INVALID_SOCKET) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;

        if (bind(rtp_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closesocket(rtp_sock);
            rtp_sock = INVALID_SOCKET;
            return false;
        }

        int len = sizeof(addr);
        getsockname(rtp_sock, reinterpret_cast<sockaddr*>(&addr), &len);
        rtp_port = ntohs(addr.sin_port);

        DWORD timeout = 1000;
        setsockopt(rtp_sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        int buf_sz = 2 * 1024 * 1024;
        setsockopt(rtp_sock, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&buf_sz), sizeof(buf_sz));
        return true;
    }

    bool setup_rtsp_listener(uint16_t port) {
        rtsp_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rtsp_listen_sock == INVALID_SOCKET) return false;

        int opt = 1;
        setsockopt(rtsp_listen_sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(rtsp_listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::cerr << "[Miracast] RTSP bind failed on port " << port << "\n";
            closesocket(rtsp_listen_sock);
            rtsp_listen_sock = INVALID_SOCKET;
            return false;
        }

        if (listen(rtsp_listen_sock, 1) != 0) {
            closesocket(rtsp_listen_sock);
            rtsp_listen_sock = INVALID_SOCKET;
            return false;
        }

        rtsp_port = port;
        return true;
    }

    void rtp_loop() {
        std::vector<uint8_t> buf(65536);
        while (streaming.load()) {
            int n = recvfrom(rtp_sock, reinterpret_cast<char*>(buf.data()),
                             static_cast<int>(buf.size()), 0, nullptr, nullptr);
            if (n <= 12) continue;

            // RTP header parsing
            if (((buf[0] >> 6) & 3) != 2) continue; // Version must be 2
            int cc = buf[0] & 0x0F;
            bool ext = (buf[0] >> 4) & 1;
            bool padding = (buf[0] >> 5) & 1;
            int hdr_len = 12 + cc * 4;

            if (ext && n > hdr_len + 4) {
                int ext_words = (buf[hdr_len + 2] << 8) | buf[hdr_len + 3];
                hdr_len += 4 + ext_words * 4;
            }
            if (hdr_len >= n) continue;

            int payload_len = n - hdr_len;
            if (padding) payload_len -= buf[n - 1];
            if (payload_len <= 0) continue;

            demuxer.feed(buf.data() + hdr_len, payload_len);
        }
    }

    void decode_loop(media::OnVideoFrame& on_video, media::OnAudioFrame& on_audio) {
        // Wait for some initial data before trying to open stream
        while (!demuxer.has_data.load() && streaming.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        if (!streaming.load()) return;

        if (!demuxer.open_stream()) {
            std::cerr << "[Miracast] Could not open MPEG-TS for decode\n";
            return;
        }

        while (streaming.load()) {
            if (!demuxer.decode_next(on_video, on_audio)) {
                if (!streaming.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    // Accept one RTSP connection and run the WFD handshake + streaming
    void rtsp_accept_loop(media::OnVideoFrame& on_video,
                          media::OnAudioFrame& on_audio,
                          std::function<void()>& on_disconnect,
                          std::atomic<bool>& running) {
        while (running.load()) {
            // Set a timeout on accept so we can check running flag
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(rtsp_listen_sock, &fds);
            timeval tv{1, 0};
            int sel = select(0, &fds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;

            sockaddr_in client_addr{};
            int addr_len = sizeof(client_addr);
            socket_t client = accept(rtsp_listen_sock,
                reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
            if (client == INVALID_SOCKET) continue;

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
            std::cout << "[Miracast] RTSP connection from " << ip_str << "\n";

            session.sock = client;
            session.state = WfdState::Idle;
            session.rtp_port = rtp_port;
            session.cseq_out = 0;
            session.session_id.clear();
            session.presentation_url.clear();

            // Run WFD negotiation
            if (session.negotiate()) {
                // Start streaming
                streaming.store(true);
                session_active.store(true);

                rtp_thread = std::thread(&Impl::rtp_loop, this);
                decode_thread = std::thread(&Impl::decode_loop, this,
                                            std::ref(on_video), std::ref(on_audio));

                // Monitor RTSP for teardown / keep-alive
                while (streaming.load() && running.load()) {
                    fd_set rfds;
                    FD_ZERO(&rfds);
                    FD_SET(client, &rfds);
                    timeval rtv{1, 0};
                    if (select(0, &rfds, nullptr, nullptr, &rtv) > 0) {
                        if (!session.handle_message()) {
                            std::cout << "[Miracast] Session ended (TEARDOWN)\n";
                            break;
                        }
                    }
                }

                // Stop streaming
                streaming.store(false);
                demuxer.signal_eof();

                if (rtp_thread.joinable()) rtp_thread.join();
                if (decode_thread.joinable()) decode_thread.join();

                session_active.store(false);

                // Reset demuxer for next session
                demuxer.cleanup();
                demuxer.init();
                demuxer.ts_buffer.clear();
                demuxer.ts_read_pos = 0;
                demuxer.eof.store(false);
                demuxer.has_data.store(false);

                if (on_disconnect) on_disconnect();
            }

            closesocket(client);
            session.sock = INVALID_SOCK;
        }
    }

    void cleanup() {
        streaming.store(false);

        if (rtp_sock != INVALID_SOCKET) { closesocket(rtp_sock); rtp_sock = INVALID_SOCKET; }
        if (rtsp_listen_sock != INVALID_SOCKET) { closesocket(rtsp_listen_sock); rtsp_listen_sock = INVALID_SOCKET; }

        demuxer.signal_eof();
        if (rtp_thread.joinable()) rtp_thread.join();
        if (decode_thread.joinable()) decode_thread.join();
        if (rtsp_thread.joinable()) rtsp_thread.join();

        if (session.sock != INVALID_SOCK) { closesocket(session.sock); session.sock = INVALID_SOCK; }
        demuxer.cleanup();
    }
};

// ============================================================================
// Public API
// ============================================================================

MiracastReceiver::MiracastReceiver() : impl_(std::make_unique<Impl>()) {}
MiracastReceiver::~MiracastReceiver() { stop(); }

bool MiracastReceiver::start(const Config& config) {
    if (running_.load()) return true;

    if (!impl_->setup_rtp_socket()) {
        std::cerr << "[Miracast] Failed to create RTP socket\n";
        return false;
    }

    if (!impl_->setup_rtsp_listener(config.rtsp_port)) {
        std::cerr << "[Miracast] Failed to start RTSP listener on port "
                  << config.rtsp_port << "\n";
        return false;
    }

    if (!impl_->demuxer.init()) {
        std::cerr << "[Miracast] Failed to init demuxer\n";
        return false;
    }

    std::promise<bool> init_promise;
    auto init_future = init_promise.get_future();

    worker_thread_ = std::thread([this, config, &init_promise]() {
        impl_->worker_thread_id = GetCurrentThreadId();

        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);

            // Create WiFi Direct advertisement
            impl_->publisher = WFDir::WiFiDirectAdvertisementPublisher();
            auto adv = impl_->publisher.Advertisement();
            adv.IsAutonomousGroupOwnerEnabled(true);
            adv.ListenStateDiscoverability(
                WFDir::WiFiDirectAdvertisementListenStateDiscoverability::Normal);

            // WFD Information Element (advertises us as a Miracast sink)
            {
                auto ie = WFDir::WiFiDirectInformationElement();

                // Wi-Fi Alliance OUI: 50-6F-9A
                auto oui_writer = WSS::DataWriter();
                oui_writer.WriteByte(0x50);
                oui_writer.WriteByte(0x6F);
                oui_writer.WriteByte(0x9A);
                ie.Oui(oui_writer.DetachBuffer());
                ie.OuiType(0x0A); // WFD

                // WFD Device Information subelement
                auto body = WSS::DataWriter();
                body.ByteOrder(WSS::ByteOrder::BigEndian);
                body.WriteByte(0x00);          // Subelement ID: Device Info
                body.WriteUInt16(0x0006);      // Length: 6 bytes
                // Bits: [1:0]=01 (Primary Sink), [4]=1 (Session Available)
                body.WriteUInt16(0x0011);
                body.WriteUInt16(config.rtsp_port);  // RTSP control port
                body.WriteUInt16(300);         // Max throughput (Mbps)
                ie.Value(body.DetachBuffer());

                adv.InformationElements().Append(ie);
            }

            // Connection listener
            impl_->conn_listener = WFDir::WiFiDirectConnectionListener();
            impl_->conn_req_token = impl_->conn_listener.ConnectionRequested(
                [this](const WFDir::WiFiDirectConnectionListener&,
                       const WFDir::WiFiDirectConnectionRequestedEventArgs& args) {
                    auto req = args.GetConnectionRequest();
                    auto dev_info = req.DeviceInformation();
                    std::wcout << L"[Miracast] P2P request from: "
                               << dev_info.Name().c_str() << L"\n";

                    try {
                        auto params = WFDir::WiFiDirectConnectionParameters();
                        params.GroupOwnerIntent(15);

                        auto device = WFDir::WiFiDirectDevice::FromIdAsync(
                            dev_info.Id(), params).get();

                        if (device) {
                            auto eps = device.GetConnectionEndpointPairs();
                            if (eps.Size() > 0) {
                                std::wcout << L"[Miracast] P2P link established ("
                                           << eps.GetAt(0).LocalHostName().DisplayName().c_str()
                                           << L" <-> "
                                           << eps.GetAt(0).RemoteHostName().DisplayName().c_str()
                                           << L")\n";
                            }
                        }
                    } catch (const winrt::hresult_error& ex) {
                        std::wcerr << L"[Miracast] P2P connect error: "
                                   << ex.message().c_str() << L"\n";
                    }
                }
            );

            // Publisher status monitor
            impl_->pub_status_token = impl_->publisher.StatusChanged(
                [](const WFDir::WiFiDirectAdvertisementPublisher& pub,
                   const WFDir::WiFiDirectAdvertisementPublisherStatusChangedEventArgs& args) {
                    switch (pub.Status()) {
                    case WFDir::WiFiDirectAdvertisementPublisherStatus::Started:
                        std::cout << "[Miracast] WiFi Direct advertising (WFD sink)\n";
                        break;
                    case WFDir::WiFiDirectAdvertisementPublisherStatus::Aborted:
                        std::cerr << "[Miracast] WiFi Direct aborted (error="
                                  << static_cast<int>(args.Error()) << ")\n";
                        if (args.Error() == WFDir::WiFiDirectError::RadioNotAvailable)
                            std::cerr << "[Miracast] No WiFi Direct adapter found\n";
                        break;
                    case WFDir::WiFiDirectAdvertisementPublisherStatus::Stopped:
                        std::cout << "[Miracast] WiFi Direct stopped\n";
                        break;
                    default: break;
                    }
                }
            );

            impl_->publisher.Start();
            running_.store(true);

            std::cout << "[Miracast] Started as '" << config.display_name
                      << "' (WiFi Direct WFD sink)\n"
                      << "[Miracast] RTSP port " << config.rtsp_port
                      << ", RTP port " << impl_->rtp_port << "\n"
                      << "[Miracast] No 'Projecting to this PC' setting required\n";

            // Start RTSP accept thread (handles sessions independently)
            impl_->rtsp_thread = std::thread(
                &Impl::rtsp_accept_loop, impl_.get(),
                std::ref(on_video_), std::ref(on_audio_),
                std::ref(on_disconnect_), std::ref(running_));

            init_promise.set_value(true);

            // STA message loop for WiFi Direct events
            MSG msg;
            while (running_.load() && GetMessage(&msg, nullptr, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            impl_->publisher.Stop();
            winrt::uninit_apartment();

        } catch (const winrt::hresult_error& ex) {
            auto code = static_cast<uint32_t>(ex.code());
            std::wcerr << L"[Miracast] Error: " << ex.message().c_str()
                       << L" (0x" << std::hex << code << L")\n";

            if (code == 0x8007001F || code == 0x80070015) {
                std::cerr << "[Miracast] WiFi Direct unavailable — "
                          << "check that your WiFi adapter supports P2P\n";
            }

            try { init_promise.set_value(false); } catch (...) {}
        } catch (const std::exception& ex) {
            std::cerr << "[Miracast] Error: " << ex.what() << "\n";
            try { init_promise.set_value(false); } catch (...) {}
        } catch (...) {
            std::cerr << "[Miracast] Unknown error\n";
            try { init_promise.set_value(false); } catch (...) {}
        }
    });

    bool ok = init_future.get();
    if (!ok) stop();
    return ok;
}

void MiracastReceiver::stop() {
    if (!running_.load()) return;
    running_.store(false);

    impl_->cleanup();

    if (worker_thread_.joinable()) {
        if (impl_->worker_thread_id)
            PostThreadMessageW(impl_->worker_thread_id, WM_QUIT, 0, 0);
        worker_thread_.join();
    }

    std::cout << "[Miracast] Receiver stopped\n";
}

} // namespace opm::miracast

#endif // ENABLE_MIRACAST
