#include <openmirror/airplay/airplay_server.h>
#include <openmirror/airplay/bplist.h>
#include <openmirror/config.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <random>
#include <sstream>
#include <cinttypes>
#include <openssl/evp.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace openmirror::airplay {

AirPlayServer::AirPlayServer() = default;

AirPlayServer::~AirPlayServer() {
    stop();
}

void AirPlayServer::set_video_callback(media::OnVideoFrame cb) {
    on_video_ = std::move(cb);
    decoder_.set_video_callback(on_video_);
}

void AirPlayServer::set_audio_callback(media::OnAudioFrame cb) {
    on_audio_ = std::move(cb);
    decoder_.set_audio_callback(on_audio_);
}

bool AirPlayServer::start(const Config& config) {
    config_ = config;

    // Generate a random MAC address for device identity
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (int i = 0; i < 6; i++) {
        hw_addr_[i] = static_cast<uint8_t>(dist(gen));
    }
    hw_addr_[0] &= 0xFE; // unicast
    hw_addr_[0] |= 0x02; // locally administered

    // Initialize Ed25519 keypair for pairing
    if (!pairing_.init()) {
        std::cerr << "[AirPlay] Failed to init pairing crypto\n";
        return false;
    }

    // Initialize decoder for H.264 (AirPlay mirroring uses H.264)
    if (!decoder_.init_video(AV_CODEC_ID_H264)) {
        std::cerr << "[AirPlay] Failed to init video decoder\n";
        return false;
    }

    if (!decoder_.init_audio(AV_CODEC_ID_AAC, 44100, 2)) {
        std::cerr << "[AirPlay] Failed to init audio decoder\n";
        // Audio failure is non-fatal, continue without audio
    }

    // Register RTSP handlers for AirPlay control
    // AirPlay uses non-standard RTSP methods
    rtsp_.on_method("GET", [this](const auto& req) { return handle_info(req); });
    rtsp_.on_method("POST", [this](const auto& req) -> network::RtspResponse {
        try {
            // Route POST based on URI — order matters: more-specific paths first.
            if (req.uri.find("/pair-pin-start") != std::string::npos)
                return handle_pair_pin_start(req);
            if (req.uri.find("/pair-setup-pin") != std::string::npos)
                return handle_pair_setup_pin(req);
            if (req.uri.find("/pair-setup") != std::string::npos)
                return handle_pair_setup(req);
            if (req.uri.find("/pair-verify") != std::string::npos)
                return handle_pair_verify(req);
            if (req.uri.find("/fp-setup") != std::string::npos)
                return handle_fp_setup(req);
            if (req.uri.find("/feedback") != std::string::npos) {
                network::RtspResponse resp;
                resp.status_code = 200;
                return resp;
            }
            return handle_default(req);
        } catch (const std::exception& e) {
            std::cerr << "[AirPlay] EXCEPTION in POST handler (" << req.uri
                      << "): " << e.what() << "\n";
            network::RtspResponse resp;
            resp.status_code = 500;
            return resp;
        } catch (...) {
            std::cerr << "[AirPlay] UNKNOWN EXCEPTION in POST handler ("
                      << req.uri << ")\n";
            network::RtspResponse resp;
            resp.status_code = 500;
            return resp;
        }
    });
    rtsp_.on_method("SETUP", [this](const auto& req) { return handle_setup(req); });
    rtsp_.on_method("RECORD", [this](const auto& req) {
        network::RtspResponse resp;
        resp.status_code = 200;
        resp.headers["Audio-Latency"] = "11025";
        resp.headers["Audio-Jack-Status"] = "connected; type=digital";
        std::cout << "[AirPlay] RECORD — streaming started\n";
        return resp;
    });
    rtsp_.on_method("GET_PARAMETER", [this](const auto& req) { return handle_get_parameter(req); });
    rtsp_.on_method("SET_PARAMETER", [this](const auto& req) { return handle_set_parameter(req); });
    rtsp_.on_method("TEARDOWN", [this](const auto& req) { return handle_teardown(req); });
    rtsp_.set_default_handler([this](const auto& req) { return handle_default(req); });

    // Start RTSP control server
    if (!rtsp_.start(config_.port)) {
        std::cerr << "[AirPlay] Failed to start RTSP server\n";
        return false;
    }

    // Start mirror data receiver
    running_.store(true);
    mirror_thread_ = std::thread(&AirPlayServer::mirror_receive_loop, this);

    // Start event and timing listeners (used by SETUP response)
    if (!start_event_listener()) {
        std::cerr << "[AirPlay] Warning: event listener failed to start\n";
    }
    if (!start_timing_listener()) {
        std::cerr << "[AirPlay] Warning: timing listener failed to start\n";
    }

    // Advertise via mDNS
    if (!mdns_.register_airplay(config_.server_name, config_.port, hw_addr_, require_pin_)) {
        std::cerr << "[AirPlay] Warning: mDNS registration failed, "
                  << "devices may not discover this receiver automatically\n";
    }

    std::cout << "[AirPlay] Server started: " << config_.server_name
              << " (port " << config_.port << ")\n";
    return true;
}

void AirPlayServer::stop() {
    running_.store(false);
    mdns_.unregister();
    rtsp_.stop();
    if (mirror_thread_.joinable()) {
        mirror_thread_.join();
    }
    if (event_sock_ != INVALID_SOCK) {
        network::TcpServer::close_socket(event_sock_);
        event_sock_ = INVALID_SOCK;
    }
    if (event_thread_.joinable()) {
        event_thread_.join();
    }
    if (timing_sock_ != INVALID_SOCK) {
        closesocket(timing_sock_);
        timing_sock_ = INVALID_SOCK;
    }
    if (timing_thread_.joinable()) {
        timing_thread_.join();
    }
    decoder_.flush();
    std::cout << "[AirPlay] Server stopped\n";
}

// --- RTSP Handlers ---

network::RtspResponse AirPlayServer::handle_info(const network::RtspRequest& req) {
    network::RtspResponse resp;
    resp.status_code = 200;

    std::string device_id;
    {
        std::ostringstream oss;
        for (int i = 0; i < 6; i++) {
            if (i > 0) oss << ':';
            oss << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(hw_addr_[i]);
        }
        device_id = oss.str();
    }

    // Build binary plist response matching Apple TV capabilities
    BPlistWriter w;
    std::vector<std::pair<int, int>> root;

    // deviceID
    root.push_back({w.add_string("deviceID"), w.add_string(device_id)});

    // macAddress (same as deviceID for AirPlay)
    root.push_back({w.add_string("macAddress"), w.add_string(device_id)});

    // features — 64-bit (low 32 | high 32)
    // 0x5A7FFEE6 = low 32, 0x1E = high 32 (supports FairPlay, etc.)
    uint64_t features = 0x5A7FFEE6;
    root.push_back({w.add_string("features"), w.add_uint(features)});

    // model
    root.push_back({w.add_string("model"), w.add_string("AppleTV3,2")});

    // name
    root.push_back({w.add_string("name"), w.add_string(config_.server_name)});

    // protovers
    root.push_back({w.add_string("protovers"), w.add_string("1.1")});

    // sourceVersion
    root.push_back({w.add_string("sourceVersion"), w.add_string("220.68")});

    // pk (32 bytes raw Ed25519 public key)
    const char* pk_hex = "b07727d6f6cd6e08b58ede525ec3cdeaa252ad9f683feb212ef8a205246554e7";
    std::vector<uint8_t> pk_raw;
    for (int i = 0; i < 64; i += 2) {
        char hex[3] = {pk_hex[i], pk_hex[i+1], 0};
        pk_raw.push_back(static_cast<uint8_t>(strtol(hex, nullptr, 16)));
    }
    root.push_back({w.add_string("pk"), w.add_data(pk_raw.data(), pk_raw.size())});

    // pi
    root.push_back({w.add_string("pi"), w.add_string("2e388006-13ba-4041-9a67-25dd4a43d536")});

    // vv
    root.push_back({w.add_string("vv"), w.add_uint(2)});

    // statusFlags
    root.push_back({w.add_string("statusFlags"), w.add_uint(68)});

    // keepAliveLowPower
    root.push_back({w.add_string("keepAliveLowPower"), w.add_uint(1)});

    // keepAliveSendStatsAsBody
    root.push_back({w.add_string("keepAliveSendStatsAsBody"), w.add_bool(true)});

    // initialVolume
    root.push_back({w.add_string("initialVolume"), w.add_real(-30.0)});

    // audioFormats — two format entries (type 100 and 101)
    {
        auto mk = [&](const std::string& k) { return w.add_string(k); };

        auto fmt0_type = w.add_uint(100);
        auto fmt0_in = w.add_uint(0x03FFFFFC);
        auto fmt0_out = w.add_uint(0x03FFFFFC);
        auto fmt0 = w.add_dict({
            {mk("type"), fmt0_type},
            {mk("audioInputFormats"), fmt0_in},
            {mk("audioOutputFormats"), fmt0_out}
        });

        auto fmt1_type = w.add_uint(101);
        auto fmt1_in = w.add_uint(0x03FFFFFC);
        auto fmt1_out = w.add_uint(0x03FFFFFC);
        auto fmt1 = w.add_dict({
            {mk("type"), fmt1_type},
            {mk("audioInputFormats"), fmt1_in},
            {mk("audioOutputFormats"), fmt1_out}
        });

        root.push_back({mk("audioFormats"), w.add_array({fmt0, fmt1})});
    }

    // audioLatencies
    {
        auto mk = [&](const std::string& k) { return w.add_string(k); };

        auto lat0 = w.add_dict({
            {mk("type"), w.add_uint(100)},
            {mk("audioType"), w.add_string("default")},
            {mk("inputLatencyMicros"), w.add_uint(0)},
            {mk("outputLatencyMicros"), w.add_bool(false)}
        });

        auto lat1 = w.add_dict({
            {mk("type"), w.add_uint(101)},
            {mk("audioType"), w.add_string("default")},
            {mk("inputLatencyMicros"), w.add_uint(0)},
            {mk("outputLatencyMicros"), w.add_bool(false)}
        });

        root.push_back({mk("audioLatencies"), w.add_array({lat0, lat1})});
    }

    // displays — one display entry
    {
        auto mk = [&](const std::string& k) { return w.add_string(k); };

        auto disp = w.add_dict({
            {mk("uuid"), w.add_string("e0ff8a27-6738-3d56-8a16-cc53aacee925")},
            {mk("width"), w.add_uint(1920)},
            {mk("height"), w.add_uint(1080)},
            {mk("widthPixels"), w.add_uint(1920)},
            {mk("heightPixels"), w.add_uint(1080)},
            {mk("widthPhysical"), w.add_uint(0)},
            {mk("heightPhysical"), w.add_uint(0)},
            {mk("rotation"), w.add_bool(false)},
            {mk("refreshRate"), w.add_real(1.0 / 60.0)},
            {mk("maxFPS"), w.add_uint(30)},
            {mk("overscanned"), w.add_bool(false)},
            {mk("features"), w.add_uint(14)}
        });

        root.push_back({mk("displays"), w.add_array({disp})});
    }

    auto root_dict = w.add_dict(root);
    resp.body = w.build(root_dict);
    resp.headers["Content-Type"] = "application/x-apple-binary-plist";

    std::cout << "[AirPlay] Info request from client (" << resp.body.size() << " bytes bplist)\n";
    return resp;
}

network::RtspResponse AirPlayServer::handle_pair_setup(const network::RtspRequest& req) {
    network::RtspResponse resp;

    auto response_data = pairing_.pair_setup(req.body.data(),
                                              static_cast<int>(req.body.size()));
    if (response_data.empty()) {
        resp.status_code = 500;
        resp.reason = "Internal Server Error";
        std::cerr << "[AirPlay] pair-setup failed\n";
        return resp;
    }

    resp.status_code = 200;
    resp.headers["Content-Type"] = "application/octet-stream";
    resp.body = std::move(response_data);

    std::cout << "[AirPlay] Pair-setup: returned " << resp.body.size() << "-byte Ed25519 public key\n";
    return resp;
}

network::RtspResponse AirPlayServer::handle_pair_verify(const network::RtspRequest& req) {
    network::RtspResponse resp;
    resp.status_code = 200;

    if (req.body.size() < 4) {
        resp.status_code = 400;
        return resp;
    }

    uint8_t phase = req.body[0];

    if (phase == 1) {
        // Phase 1: X25519 ECDH key exchange + Ed25519 signature
        auto response_data = pairing_.pair_verify_phase1(req.body.data(),
                                                          static_cast<int>(req.body.size()));
        if (response_data.empty()) {
            resp.status_code = 500;
            std::cerr << "[AirPlay] pair-verify phase 1 failed\n";
            return resp;
        }
        resp.headers["Content-Type"] = "application/octet-stream";
        resp.body = std::move(response_data);
        std::cout << "[AirPlay] Pair-verify phase 1: ECDH exchange (" << resp.body.size() << " bytes)\n";
    } else if (phase == 0) {
        // Phase 2: Verify client's signature
        if (!pairing_.pair_verify_phase2(req.body.data(),
                                          static_cast<int>(req.body.size()))) {
            resp.status_code = 500;
            std::cerr << "[AirPlay] pair-verify phase 2 failed\n";
            return resp;
        }
        resp.headers["Content-Type"] = "application/octet-stream";
        std::cout << "[AirPlay] Pair-verify phase 2: verified OK\n";
    } else {
        std::cerr << "[AirPlay] pair-verify: unknown phase " << (int)phase << "\n";
        resp.status_code = 400;
    }

    return resp;
}

// ---- AirPlay 1 PIN pairing (SRP-6a + AES-CBC) ----
//
// /pair-pin-start (POST, empty body) — server generates 4-digit PIN and
//   shows it on screen. Always returns 200.
//
// /pair-setup-pin (POST) — three sequential binary-plist exchanges:
//   step 1 in : { "method": "pin", "user": "Pair-Setup" }
//   step 1 out: { "pk": B (256), "salt": s (16) }
//   step 2 in : { "pk": A (256), "proof": M1 (20) }
//   step 2 out: { "proof": M2 (20) }
//   step 3 in : { "epk": ENC(client LTPK), "authTag": ... }
//   step 3 out: { "epk": ENC(server LTPK), "authTag": ... }

network::RtspResponse AirPlayServer::handle_pair_pin_start(const network::RtspRequest& /*req*/) {
    network::RtspResponse resp;
    resp.status_code = 200;

    // Generate 4-digit PIN.
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 9999);
    char pin[5];
    std::snprintf(pin, sizeof(pin), "%04d", dist(gen));

    {
        std::lock_guard<std::mutex> lk(pin_mutex_);
        current_pin_ = pin;
        srp_active_ = false; // forced fresh start on next /pair-setup-pin
    }

    if (on_pin_display_) on_pin_display_(pin);
    std::cout << "[AirPlay] /pair-pin-start — PIN = " << pin << "\n";

    return resp;
}

network::RtspResponse AirPlayServer::handle_pair_setup_pin(const network::RtspRequest& req) {
    network::RtspResponse resp;
    resp.status_code = 200;
    resp.headers["Content-Type"] = "application/x-apple-binary-plist";

    BPlistReader r;
    if (!r.parse(req.body.data(), req.body.size())) {
        std::cerr << "[AirPlay] pair-setup-pin: body is not a binary plist ("
                  << req.body.size() << " bytes)\n";
        // Hex-dump the first 256 bytes so we can see what iOS sent.
        std::cerr << "[AirPlay] body hex:";
        size_t n = std::min<size_t>(256, req.body.size());
        for (size_t i = 0; i < n; i++) {
            char hb[4]; std::snprintf(hb, 4, "%02x", req.body[i]);
            std::cerr << (i % 32 == 0 ? "\n  " : " ") << hb;
        }
        std::cerr << std::endl;
        resp.status_code = 400;
        return resp;
    }
    // Serialize the entire SRP/PIN state machine — RTSP runs each client on
    // its own thread, and srp_pin_ holds non-thread-safe BIGNUM state.
    std::lock_guard<std::mutex> lk(pin_mutex_);

    // ----- Step 1: client requests SRP parameters -----
    if (r.has_key("method") && r.has_key("user")) {
        std::string method, user;
        r.get_string("method", method);
        r.get_string("user", user);
        std::cout << "[AirPlay] pair-setup-pin step 1 — method=" << method
                  << " user=" << user << "\n";

        if (current_pin_.empty()) {
            std::cerr << "[AirPlay] pair-setup-pin: no active PIN — call /pair-pin-start first\n";
            resp.status_code = 400;
            return resp;
        }

        // SRP username "I" is the CLIENT-supplied "user" field (iPad's own
        // device-id, e.g. its MAC). iOS computes x = H(s | H(I | ":" | PIN))
        // using this same I, so the server MUST use the value the client sent.
        const std::string& srp_username = user;
        if (!srp_pin_.start(current_pin_, srp_username)) {
            std::cerr << "[AirPlay] SRP start failed\n";
            resp.status_code = 500;
            return resp;
        }
        srp_active_ = true;

        auto salt = srp_pin_.get_salt();
        auto B    = srp_pin_.get_B();

        BPlistWriter w;
        std::vector<std::pair<int, int>> root;
        // Order matches UxPlay: pk first, then salt.
        root.push_back({w.add_string("pk"),   w.add_data(B.data(),    B.size())});
        root.push_back({w.add_string("salt"), w.add_data(salt.data(), salt.size())});
        resp.body = w.build(w.add_dict(root));

        std::cout << "[AirPlay] pair-setup-pin step 1 → salt(" << salt.size()
                  << ") + B(" << B.size() << ")\n";
        return resp;
    }

    // ----- Step 2: client sends A + M1 -----
    if (r.has_key("pk") && r.has_key("proof")) {
        if (!srp_active_) {
            std::cerr << "[AirPlay] pair-setup-pin step 2 without active SRP\n";
            resp.status_code = 400;
            return resp;
        }
        std::vector<uint8_t> A, M1;
        r.get_data("pk",    A);
        r.get_data("proof", M1);
        std::cout << "[AirPlay] pair-setup-pin step 2 — A(" << A.size()
                  << ") M1(" << M1.size() << ")" << std::endl;
        if (A.size() != 256 || M1.size() != 20) {
            std::cerr << "[AirPlay] pair-setup-pin step 2: invalid sizes — abort" << std::endl;
            srp_active_ = false;
            resp.status_code = 400;
            return resp;
        }

        bool ok;
        try {
            ok = srp_pin_.process_client_pubkey(A, M1);
        } catch (const std::exception& e) {
            std::cerr << "[AirPlay] step 2: process_client_pubkey threw: "
                      << e.what() << std::endl;
            srp_active_ = false;
            resp.status_code = 500;
            return resp;
        }
        if (!ok) {
            std::cerr << "[AirPlay] SRP M1 verification failed (wrong PIN)" << std::endl;
            if (on_pin_display_) on_pin_display_("");
            srp_active_ = false;
            resp.status_code = 470; // AirPlay "auth required / wrong PIN"
            return resp;
        }

        auto M2 = srp_pin_.get_M2();
        BPlistWriter w;
        std::vector<std::pair<int, int>> root;
        root.push_back({w.add_string("proof"), w.add_data(M2.data(), M2.size())});
        resp.body = w.build(w.add_dict(root));

        std::cout << "[AirPlay] pair-setup-pin step 2 → M2(" << M2.size()
                  << ") — PIN accepted" << std::endl;
        return resp;
    }

    // ----- Step 3: client sends encrypted LTPK -----
    if (r.has_key("epk")) {
        if (!srp_active_ || !srp_pin_.is_authenticated()) {
            std::cerr << "[AirPlay] pair-setup-pin step 3 without authenticated SRP\n";
            resp.status_code = 400;
            return resp;
        }

        std::vector<uint8_t> epk, authTag;
        r.get_data("epk", epk);
        r.get_data("authTag", authTag);
        std::cout << "[AirPlay] pair-setup-pin step 3 — epk(" << epk.size()
                  << ") authTag(" << authTag.size() << ")\n";

        auto K = srp_pin_.get_session_key();
        auto client_ltpk = srp_pin_decrypt_ltpk(K, epk);
        if (client_ltpk.size() < 32) {
            std::cerr << "[AirPlay] step 3: failed to decrypt client LTPK ("
                      << client_ltpk.size() << " bytes)\n";
            resp.status_code = 470;
            return resp;
        }
        std::cout << "[AirPlay] step 3 — decrypted client LTPK ("
                  << client_ltpk.size() << " bytes)\n";

        auto our_ltpk = pairing_.get_public_key();
        auto our_epk  = srp_pin_encrypt_ltpk(K, our_ltpk);
        if (our_epk.empty()) {
            std::cerr << "[AirPlay] step 3: failed to encrypt our LTPK\n";
            resp.status_code = 500;
            return resp;
        }

        BPlistWriter w;
        std::vector<std::pair<int, int>> root;
        root.push_back({w.add_string("epk"), w.add_data(our_epk.data(), our_epk.size())});
        // authTag echo (AirPlay 1 doesn't actually GCM-tag here; many clients ignore).
        if (!authTag.empty())
            root.push_back({w.add_string("authTag"), w.add_data(authTag.data(), authTag.size())});
        resp.body = w.build(w.add_dict(root));

        // Pairing complete — clear PIN from screen.
        if (on_pin_display_) on_pin_display_("");
        current_pin_.clear();
        srp_active_ = false;

        std::cout << "[AirPlay] PIN pairing complete — sent encrypted server LTPK\n";
        return resp;
    }

    std::cerr << "[AirPlay] pair-setup-pin: unrecognized body keys\n";
    resp.status_code = 400;
    return resp;
}

network::RtspResponse AirPlayServer::handle_fp_setup(const network::RtspRequest& req) {
    network::RtspResponse resp;
    resp.status_code = 200;

    int datalen = static_cast<int>(req.body.size());

    if (datalen == 16) {
        // Phase 1: FairPlay challenge → 142-byte response
        auto response_data = fairplay_.setup(req.body.data(), datalen);
        if (response_data.empty()) {
            resp.status_code = 500;
            return resp;
        }
        resp.headers["Content-Type"] = "application/octet-stream";
        resp.body = std::move(response_data);
        std::cout << "[AirPlay] FP-setup phase 1 → " << resp.body.size() << " bytes\n";
    } else if (datalen == 164) {
        // Phase 2: FairPlay handshake → 32-byte response
        auto response_data = fairplay_.handshake(req.body.data(), datalen);
        if (response_data.empty()) {
            resp.status_code = 500;
            return resp;
        }
        resp.headers["Content-Type"] = "application/octet-stream";
        resp.body = std::move(response_data);
        std::cout << "[AirPlay] FP-setup phase 2 → " << resp.body.size() << " bytes\n";
    } else {
        std::cerr << "[AirPlay] FP-setup: unexpected data length " << datalen << "\n";
        resp.status_code = 400;
    }

    return resp;
}

network::RtspResponse AirPlayServer::handle_setup(const network::RtspRequest& req) {
    network::RtspResponse resp;
    resp.status_code = 200;

    // AirPlay SETUP uses binary plist bodies
    BPlistReader reader;
    if (req.body.empty() || !reader.parse(req.body.data(), req.body.size())) {
        // Legacy RTSP SETUP (non-plist body)
        resp.headers["Transport"] = "RTP/AVP/TCP;unicast;interleaved=0-1;mode=record;"
                                    "server_port=" + std::to_string(config_.mirror_port);
        std::cout << "[AirPlay] SETUP (legacy) — mirror port: " << config_.mirror_port << "\n";
        return resp;
    }

    resp.headers["Content-Type"] = "application/x-apple-binary-plist";

    BPlistWriter writer;
    std::vector<std::pair<int, int>> root_entries;

    // Phase 1: Encryption setup (eiv + ekey present)
    std::vector<uint8_t> eiv, ekey;
    if (reader.get_data("eiv", eiv) && reader.get_data("ekey", ekey)) {
        std::cout << "[AirPlay] SETUP: encryption init (eiv=" << eiv.size()
                  << "B, ekey=" << ekey.size() << "B)\n";

        uint64_t timing_rport = 0;
        reader.get_uint("timingPort", timing_rport);
        std::cout << "[AirPlay] SETUP: client timingPort=" << timing_rport << "\n";

        // Try to decrypt the AES stream key via FairPlay
        if (ekey.size() == 72) {
            std::cout << "[AirPlay] ekey(" << ekey.size() << "): ";
            for (size_t i = 0; i < ekey.size(); i++)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)ekey[i];
            std::cout << "\n[AirPlay] eiv(" << eiv.size() << "): ";
            for (size_t i = 0; i < eiv.size(); i++)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)eiv[i];
            std::cout << std::dec << "\n";

            if (fairplay_.decrypt(ekey.data(), aes_key_)) {
                // Hash AES key with ECDH shared secret (SHA-512, first 16 bytes), matching UxPlay
                uint8_t ecdh_secret[32];
                if (pairing_.get_ecdh_secret(ecdh_secret)) {
                    uint8_t sha512_out[64];
                    unsigned int sha_len = 0;
                    EVP_MD_CTX* md = EVP_MD_CTX_new();
                    EVP_DigestInit_ex(md, EVP_sha512(), nullptr);
                    EVP_DigestUpdate(md, aes_key_, 16);
                    EVP_DigestUpdate(md, ecdh_secret, 32);
                    EVP_DigestFinal_ex(md, sha512_out, &sha_len);
                    EVP_MD_CTX_free(md);
                    memcpy(aes_key_, sha512_out, 16);
                    std::cout << "[AirPlay] AES key hashed with ECDH secret (SHA-512)\n";
                }
                has_aes_key_ = true;
                mirror_buffer_.set_aes_key(aes_key_);
                std::cout << "[AirPlay] FairPlay: AES stream key decrypted OK\n";
            } else {
                std::cout << "[AirPlay] NOTE: FairPlay decrypt failed — "
                          << "stream will be encrypted\n";
            }
        }

        // Respond with our ports
        auto k1 = writer.add_string("eventPort");
        auto v1 = writer.add_uint(event_port_);
        root_entries.push_back({k1, v1});

        auto k2 = writer.add_string("timingPort");
        auto v2 = writer.add_uint(timing_port_);
        root_entries.push_back({k2, v2});

        std::cout << "[AirPlay] SETUP response: eventPort=" << event_port_
                  << " timingPort=" << timing_port_ << "\n";
    }

    // Phase 2: Stream setup (streams array present)
    std::vector<BPlistReader::StreamInfo> streams;
    if (reader.get_streams(streams)) {
        std::vector<int> stream_dicts;

        for (const auto& s : streams) {
            std::cout << "[AirPlay] SETUP stream type=" << s.type;
            if (s.stream_connection_id)
                std::cout << " connID=" << s.stream_connection_id;
            std::cout << "\n";

            if (s.type == 110) {
                // Mirror stream — store connection ID and init AES
                if (s.stream_connection_id) {
                    stream_connection_id_ = s.stream_connection_id;
                    mirror_buffer_.init_aes(stream_connection_id_);
                }
                auto k_dp = writer.add_string("dataPort");
                auto v_dp = writer.add_uint(config_.mirror_port);
                auto k_ty = writer.add_string("type");
                auto v_ty = writer.add_uint(110);
                auto d = writer.add_dict({{k_dp, v_dp}, {k_ty, v_ty}});
                stream_dicts.push_back(d);
            } else if (s.type == 96) {
                // Audio stream (stub — return port 0 for now)
                auto k_dp = writer.add_string("dataPort");
                auto v_dp = writer.add_uint(0);
                auto k_cp = writer.add_string("controlPort");
                auto v_cp = writer.add_uint(0);
                auto k_ty = writer.add_string("type");
                auto v_ty = writer.add_uint(96);
                auto d = writer.add_dict({{k_dp, v_dp}, {k_cp, v_cp}, {k_ty, v_ty}});
                stream_dicts.push_back(d);
            }
        }

        if (!stream_dicts.empty()) {
            auto arr = writer.add_array(stream_dicts);
            auto k_streams = writer.add_string("streams");
            root_entries.push_back({k_streams, arr});
        }
    }

    if (!root_entries.empty()) {
        auto root = writer.add_dict(root_entries);
        resp.body = writer.build(root);
        std::cout << "[AirPlay] SETUP response: " << resp.body.size() << " bytes bplist\n";
    }

    return resp;
}

network::RtspResponse AirPlayServer::handle_get_parameter(const network::RtspRequest& req) {
    network::RtspResponse resp;
    resp.status_code = 200;

    // Check Content-Type to determine response format
    auto ct_it = req.headers.find("Content-Type");
    if (ct_it == req.headers.end()) {
        // No Content-Type — just return 200 OK (keepalive probe)
        return resp;
    }

    if (ct_it->second.find("text/parameters") != std::string::npos) {
        // Parse text parameter queries
        std::string body_str(req.body.begin(), req.body.end());
        if (body_str.find("volume") != std::string::npos) {
            std::string vol_resp = "volume: -30.000000\r\n";
            resp.headers["Content-Type"] = "text/parameters";
            resp.body.assign(vol_resp.begin(), vol_resp.end());
        }
    }

    return resp;
}

network::RtspResponse AirPlayServer::handle_set_parameter(const network::RtspRequest& req) {
    network::RtspResponse resp;
    resp.status_code = 200;
    // Handle volume, metadata, artwork, etc.
    return resp;
}

network::RtspResponse AirPlayServer::handle_teardown(const network::RtspRequest& req) {
    network::RtspResponse resp;
    resp.status_code = 200;
    std::cout << "[AirPlay] TEARDOWN — session ended\n";
    if (on_disconnect_) on_disconnect_();
    return resp;
}

network::RtspResponse AirPlayServer::handle_default(const network::RtspRequest& req) {
    std::cout << "[AirPlay] Unhandled: " << req.method << " " << req.uri << "\n";
    network::RtspResponse resp;
    resp.status_code = 200;
    return resp;
}

// --- Event Listener (TCP) ---

bool AirPlayServer::start_event_listener() {
    event_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (event_sock_ == INVALID_SOCK) return false;

    int opt = 1;
    setsockopt(event_sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0; // OS picks a free port

    if (bind(event_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        network::TcpServer::close_socket(event_sock_);
        event_sock_ = INVALID_SOCK;
        return false;
    }

    // Retrieve the assigned port
    sockaddr_in bound{};
    int len = sizeof(bound);
    getsockname(event_sock_, reinterpret_cast<sockaddr*>(&bound), &len);
    event_port_ = ntohs(bound.sin_port);

    if (listen(event_sock_, 4) != 0) {
        network::TcpServer::close_socket(event_sock_);
        event_sock_ = INVALID_SOCK;
        return false;
    }

    event_thread_ = std::thread(&AirPlayServer::event_accept_loop, this);
    std::cout << "[AirPlay] Event listener on port " << event_port_ << "\n";
    return true;
}

void AirPlayServer::event_accept_loop() {
    while (running_.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(event_sock_, &fds);
        timeval tv{0, 500000}; // 500ms poll
        int sel = select(static_cast<int>(event_sock_) + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in client_addr{};
        int client_len = sizeof(client_addr);
        socket_t client = accept(event_sock_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client == INVALID_SOCK) continue;

        char ip[64];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        std::cout << "[AirPlay] Event channel connected from " << ip
                  << ":" << ntohs(client_addr.sin_port) << "\n";

        // Hold the connection — just drain any data the client sends
        // (events flow server→client but we don't send any yet)
        std::thread([this, client]() {
            uint8_t buf[1024];
            while (running_.load()) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(client, &rfds);
                timeval t{1, 0};
                int r = select(static_cast<int>(client) + 1, &rfds, nullptr, nullptr, &t);
                if (r > 0) {
                    int n = recv(client, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                    if (n <= 0) break;
                    std::cout << "[AirPlay] Event channel received " << n << " bytes\n";
                }
            }
            network::TcpServer::close_socket(client);
            std::cout << "[AirPlay] Event channel disconnected\n";
        }).detach();
    }
}

// --- Timing Listener (UDP NTP) ---

bool AirPlayServer::start_timing_listener() {
    timing_sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (timing_sock_ == INVALID_SOCK) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

    if (bind(timing_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(timing_sock_);
        timing_sock_ = INVALID_SOCK;
        return false;
    }

    sockaddr_in bound{};
    int len = sizeof(bound);
    getsockname(timing_sock_, reinterpret_cast<sockaddr*>(&bound), &len);
    timing_port_ = ntohs(bound.sin_port);

    timing_thread_ = std::thread(&AirPlayServer::timing_loop, this);
    std::cout << "[AirPlay] Timing (NTP) listener on port " << timing_port_ << "\n";
    return true;
}

void AirPlayServer::timing_loop() {
    // Apple AirPlay NTP timing: 32-byte request, 32-byte response
    // Request:  [8B header] [8B padding] [8B padding] [8B origin timestamp]
    // Response: [8B header] [8B receive timestamp] [8B transmit timestamp] [8B origin (echo)]
    while (running_.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(timing_sock_, &fds);
        timeval tv{0, 500000};
        int sel = select(static_cast<int>(timing_sock_) + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        uint8_t buf[128];
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(timing_sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);

        if (n < 32) continue;

        // Build NTP-like response
        // Copy the request as base
        uint8_t reply[32];
        memset(reply, 0, sizeof(reply));

        // Header: mark as response
        reply[0] = 0x80;
        reply[1] = 0xd3; // timing response type

        // Bytes 2-3: sequence (echo from request)
        reply[2] = buf[2];
        reply[3] = buf[3];

        // Get current time as NTP timestamp (seconds since 1900 + fraction)
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        uint64_t ntp_sec = std::chrono::duration_cast<std::chrono::seconds>(epoch).count() + 2208988800ULL;
        auto frac_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count() % 1000000000LL;
        uint64_t ntp_frac = static_cast<uint64_t>(frac_ns) * 0x100000000ULL / 1000000000ULL;

        auto write_ntp = [](uint8_t* p, uint64_t sec, uint64_t frac) {
            p[0] = static_cast<uint8_t>(sec >> 24);
            p[1] = static_cast<uint8_t>(sec >> 16);
            p[2] = static_cast<uint8_t>(sec >> 8);
            p[3] = static_cast<uint8_t>(sec);
            p[4] = static_cast<uint8_t>(frac >> 24);
            p[5] = static_cast<uint8_t>(frac >> 16);
            p[6] = static_cast<uint8_t>(frac >> 8);
            p[7] = static_cast<uint8_t>(frac);
        };

        // Origin timestamp at offset 24 = echo of client's transmit (bytes 24-31)
        memcpy(reply + 24, buf + 24, 8);

        // Receive timestamp at offset 8
        write_ntp(reply + 8, ntp_sec, ntp_frac);

        // Transmit timestamp at offset 16
        write_ntp(reply + 16, ntp_sec, ntp_frac);

        sendto(timing_sock_, reinterpret_cast<const char*>(reply), 32, 0,
               reinterpret_cast<sockaddr*>(&from), from_len);

        std::cout << "[AirPlay] Timing: NTP response sent\n";
    }
}

// --- Mirror Data Receiver ---

void AirPlayServer::mirror_receive_loop() {
    // AirPlay mirror stream protocol:
    // Each packet: [128-byte header] [N-byte payload]
    //   header[0:3]  = payload size (big-endian)
    //   header[4:5]  = type: 0x0000=encrypted video, 0x0010=encrypted IDR,
    //                         0x0100=unencrypted SPS/PPS, 0x0500=streaming report
    //   header[6:7]  = flags/options
    //   header[8:15] = NTP timestamp
    //   header[16:127] = metadata (width/height for SPS/PPS packets)

    static const uint8_t nal_start_code[4] = {0x00, 0x00, 0x00, 0x01};

    network::TcpServer mirror_server;
    mirror_server.start(config_.mirror_port, [this](socket_t client, const std::string& addr) {
        std::cout << "[AirPlay] Mirror stream connected from " << addr << "\n";

        int64_t frame_count = 0;
        std::vector<uint8_t> sps_pps;
        bool prepend_sps_pps = false;
        uint64_t sps_pps_timestamp = 0;

        while (running_.load()) {
            // Read 128-byte header
            uint8_t header[128] = {};
            int hdr_read = 0;
            while (hdr_read < 128) {
                int r = network::TcpServer::recv_exact(client, header + hdr_read, 128 - hdr_read);
                if (r <= 0) goto stream_end;
                hdr_read += r;
            }

            // Parse header — payload size is LITTLE-ENDIAN
            uint32_t payload_size = static_cast<uint32_t>(header[0]) |
                                    (static_cast<uint32_t>(header[1]) << 8) |
                                    (static_cast<uint32_t>(header[2]) << 16) |
                                    (static_cast<uint32_t>(header[3]) << 24);
            uint8_t packet_type = header[4];
            uint8_t packet_subtype = header[5];
            uint8_t flag0 = header[6];
            // uint8_t flag1 = header[7];
            uint64_t ntp_timestamp = 0;
            for (int i = 0; i < 8; i++) {
                ntp_timestamp = (ntp_timestamp << 8) | header[8 + i];
            }

            if (payload_size > 10 * 1024 * 1024) {
                std::cerr << "[AirPlay] Mirror: invalid payload size " << payload_size << "\n";
                break;
            }

            // Read payload
            std::vector<uint8_t> payload;
            if (payload_size > 0) {
                payload.resize(payload_size);
                int pay_read = 0;
                while (pay_read < static_cast<int>(payload_size)) {
                    int r = network::TcpServer::recv_exact(client, payload.data() + pay_read,
                                                            static_cast<int>(payload_size) - pay_read);
                    if (r <= 0) goto stream_end;
                    pay_read += r;
                }
            }

            switch (packet_type) {
            case 0x00: {
                // Encrypted video data (VCL NAL: non-IDR or IDR)
                if (payload.empty()) break;

                std::vector<uint8_t> decrypted;
                std::vector<uint8_t> output_buf;

                // Check if we need to prepend SPS/PPS
                if (prepend_sps_pps && !sps_pps.empty()) {
                    if (ntp_timestamp != sps_pps_timestamp) {
                        std::cout << "[AirPlay] Mirror: SPS/PPS timestamp mismatch, discarding\n";
                        sps_pps.clear();
                    }
                    prepend_sps_pps = false;
                }

                // Decrypt
                if (mirror_buffer_.is_initialized()) {
                    decrypted.resize(payload_size);
                    mirror_buffer_.decrypt(payload.data(), decrypted.data(),
                                           static_cast<int>(payload_size));
                } else {
                    // No decryption available — pass through (will fail to decode)
                    decrypted = std::move(payload);
                }

                // Debug: log first 32 bytes of first few packets
                if (frame_count < 3) {
                    std::cout << "[AirPlay] Mirror pkt#" << frame_count
                              << " type=0x" << std::hex << (int)packet_type
                              << std::setfill('0') << std::setw(2) << (int)packet_subtype
                              << std::dec << " size=" << payload_size << " enc: ";
                    for (int i = 0; i < (std::min)(32, (int)payload_size); i++)
                        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)payload[i];
                    std::cout << "\n[AirPlay]  dec: ";
                    for (int i = 0; i < (std::min)(32, (int)decrypted.size()); i++)
                        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)decrypted[i];
                    // Show expected first 4 bytes (NAL size = payload_size - 4)
                    uint32_t expected_nal_size = payload_size - 4;
                    std::cout << "\n[AirPlay]  expected start: "
                              << std::hex << std::setfill('0')
                              << std::setw(2) << ((expected_nal_size >> 24) & 0xFF)
                              << std::setw(2) << ((expected_nal_size >> 16) & 0xFF)
                              << std::setw(2) << ((expected_nal_size >> 8) & 0xFF)
                              << std::setw(2) << (expected_nal_size & 0xFF)
                              << " 65(IDR) or 61(P) or 06(SEI)";
                    std::cout << std::dec << "\n";
                }

                // Prepend SPS/PPS if available
                if (!sps_pps.empty()) {
                    output_buf.reserve(sps_pps.size() + decrypted.size());
                    output_buf.insert(output_buf.end(), sps_pps.begin(), sps_pps.end());
                    sps_pps.clear();
                } else {
                    output_buf.reserve(decrypted.size());
                }

                // Replace 4-byte NAL size prefixes with 00 00 00 01 start codes
                int pos = 0;
                int total = static_cast<int>(decrypted.size());
                bool valid = true;
                while (pos < total) {
                    if (pos + 4 > total) { valid = false; break; }
                    int nalu_len = (static_cast<int>(decrypted[pos]) << 24) |
                                   (static_cast<int>(decrypted[pos + 1]) << 16) |
                                   (static_cast<int>(decrypted[pos + 2]) << 8) |
                                   static_cast<int>(decrypted[pos + 3]);
                    if (nalu_len < 0 || pos + 4 + nalu_len > total) { valid = false; break; }

                    // Check forbidden_zero_bit
                    if (decrypted[pos + 4] & 0x80) { valid = false; break; }

                    output_buf.insert(output_buf.end(), nal_start_code, nal_start_code + 4);
                    output_buf.insert(output_buf.end(),
                                      decrypted.data() + pos + 4,
                                      decrypted.data() + pos + 4 + nalu_len);
                    pos += 4 + nalu_len;
                }

                if (!valid) {
                    // Mark first byte to flag invalid data
                    if (!output_buf.empty()) output_buf[0] = 1;
                }

                decoder_.decode_video(output_buf.data(), output_buf.size(), frame_count++);
                break;
            }

            case 0x01: {
                // Unencrypted SPS + PPS NAL units
                if (payload.empty()) {
                    std::cerr << "[AirPlay] Mirror: SPS/PPS packet with no payload\n";
                    break;
                }

                // Check for h265 (hvc1 signature at offset 4)
                if (payload.size() > 8 && payload[4] == 0x68 && payload[5] == 0x76 &&
                    payload[6] == 0x63 && payload[7] == 0x31) {
                    std::cout << "[AirPlay] Mirror: H.265 detected (not yet supported)\n";
                    break;
                }

                // H.264 SPS/PPS format:
                //   payload[0:5] = header (version, profile, compat, level, etc.)
                //   payload[6:7] = SPS size (big-endian)
                //   payload[8...] = SPS data
                //   payload[8+sps_size] = PPS count
                //   payload[8+sps_size+1:2] = PPS size (big-endian)
                //   payload[8+sps_size+3...] = PPS data

                if (payload.size() < 11) {
                    std::cerr << "[AirPlay] Mirror: SPS/PPS payload too small\n";
                    break;
                }

                int sps_size = (static_cast<int>(payload[6]) << 8) | payload[7];
                if (sps_size <= 0 || 8 + sps_size + 3 > static_cast<int>(payload.size())) {
                    std::cerr << "[AirPlay] Mirror: invalid SPS size " << sps_size << "\n";
                    break;
                }

                uint8_t* sps_data = payload.data() + 8;

                int pps_offset = 8 + sps_size + 1; // +1 for PPS count byte
                if (pps_offset + 2 > static_cast<int>(payload.size())) {
                    std::cerr << "[AirPlay] Mirror: PPS offset out of bounds\n";
                    break;
                }

                int pps_size = (static_cast<int>(payload[pps_offset]) << 8) | payload[pps_offset + 1];
                if (pps_size <= 0 || pps_offset + 2 + pps_size > static_cast<int>(payload.size())) {
                    std::cerr << "[AirPlay] Mirror: invalid PPS size " << pps_size << "\n";
                    break;
                }
                uint8_t* pps_data = payload.data() + pps_offset + 2;

                // Build SPS+PPS buffer: [start_code][SPS][start_code][PPS]
                sps_pps.clear();
                sps_pps.reserve(sps_size + pps_size + 8);
                sps_pps.insert(sps_pps.end(), nal_start_code, nal_start_code + 4);
                sps_pps.insert(sps_pps.end(), sps_data, sps_data + sps_size);
                sps_pps.insert(sps_pps.end(), nal_start_code, nal_start_code + 4);
                sps_pps.insert(sps_pps.end(), pps_data, pps_data + pps_size);

                prepend_sps_pps = true;
                sps_pps_timestamp = ntp_timestamp;

                // Check if video stream is being suspended
                if (flag0 == 0x56 || flag0 == 0x5e) {
                    std::cout << "[AirPlay] Mirror: client signaled video stream pause\n";
                }

                std::cout << "[AirPlay] Mirror: SPS(" << sps_size << ")+PPS("
                          << pps_size << ") received\n";
                break;
            }

            case 0x05:
                // Streaming report (binary plist with FPS data) — ignore
                break;

            case 0x02:
                // Old protocol once-per-second packet — ignore
                break;

            default:
                std::cout << "[AirPlay] Mirror: unknown packet type 0x"
                          << std::hex << static_cast<int>(packet_type)
                          << std::dec << " size=" << payload_size << "\n";
                break;
            }
        }

stream_end:
        std::cout << "[AirPlay] Mirror stream ended (" << frame_count << " frames)\n";
        network::TcpServer::close_socket(client);
    });

    // Keep the mirror server alive while running
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    mirror_server.stop();
}

} // namespace openmirror::airplay
