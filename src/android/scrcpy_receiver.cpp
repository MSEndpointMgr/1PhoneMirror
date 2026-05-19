// Wireless Android screen mirror via the scrcpy server protocol.
//
// scrcpy server stream layout (v2.x / v3.x, video-only mode):
//   uint8[64]  device_name (NUL-padded)
//   uint32 BE  codec_id           ('h264' / 'h265' / 'av01')
//   uint32 BE  width
//   uint32 BE  height
//   --- repeating frames ---
//   uint64 BE  pts_us  (top bit  = SCRCPY_PACKET_FLAG_CONFIG,
//                       next bit = SCRCPY_PACKET_FLAG_KEY_FRAME on 2.x+)
//   uint32 BE  payload_size
//   uint8[size] H.264 / H.265 bitstream (Annex-B; SPS/PPS arrive as a
//               config-flagged packet ahead of the first key frame).

#ifdef ENABLE_ANDROID

#include <opm/android/scrcpy_receiver.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
}

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #pragma comment(lib, "ws2_32.lib")
  using socket_t = SOCKET;
  static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
  #define CLOSESOCK closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using socket_t = int;
  static constexpr socket_t INVALID_SOCK = -1;
  #define CLOSESOCK ::close
#endif

namespace opm::android {

// ----------------------------------------------------------------------------
// Process helpers (Windows). Non-Windows paths are stubs for now.
// ----------------------------------------------------------------------------

#ifdef _WIN32
namespace {

std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    for (char c : a) {
        if (c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

int spawn_capture(const std::string& exe,
                  const std::vector<std::string>& args,
                  const std::string& stdin_data,
                  std::string* out_stdout) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_r=nullptr, in_w=nullptr, out_r=nullptr, out_w=nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) return -1;
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r); CloseHandle(in_w);
        return -1;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    std::string cmdline = quote_arg(exe);
    for (auto& a : args) { cmdline += ' '; cmdline += quote_arg(a); }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = in_r;
    si.hStdOutput = out_w;
    si.hStdError  = out_w;
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
        CloseHandle(in_w); CloseHandle(out_r);
        return -1;
    }

    if (!stdin_data.empty()) {
        DWORD written = 0;
        WriteFile(in_w, stdin_data.data(), (DWORD)stdin_data.size(), &written, nullptr);
    }
    CloseHandle(in_w);

    if (out_stdout) out_stdout->clear();
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(out_r, buf, sizeof(buf), &n, nullptr) && n > 0) {
        if (out_stdout) out_stdout->append(buf, n);
    }
    CloseHandle(out_r);

    // Bound the wait so a hung adb (known to happen after a phone
    // disconnects, sleep/wake, or network change) can never freeze the
    // caller. 10 s is generous for any normal adb command. If we time
    // out we kill the child so its handles drop and we don't leak it.
    DWORD wait_rc = WaitForSingleObject(pi.hProcess, 10000);
    if (wait_rc == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

HANDLE spawn_detached(const std::string& exe,
                      const std::vector<std::string>& args) {
    std::string cmdline = quote_arg(exe);
    for (auto& a : args) { cmdline += ' '; cmdline += quote_arg(a); }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

} // namespace
#else
namespace {
int spawn_capture(const std::string&, const std::vector<std::string>&,
                  const std::string&, std::string*) { return -1; }
void* spawn_detached(const std::string&, const std::vector<std::string>&) { return nullptr; }
}
#endif

// ----------------------------------------------------------------------------
// AdbController
// ----------------------------------------------------------------------------

AdbController::AdbController() {
    // Force adb to use its built-in (openscreen) mDNS backend instead of
    // Bonjour. The Bonjour backend frequently goes stale after sleep/wake
    // or a network change and silently reports 0 entries even when the
    // phone is perfectly reachable. Openscreen is more reliable on
    // Windows. Set before the adb server is started so it inherits the
    // env var; if a server is already running with the wrong backend, the
    // pair workflow will kill_server() once to recycle it.
#ifdef _WIN32
    SetEnvironmentVariableA("ADB_MDNS_OPENSCREEN", "1");
#endif
}
AdbController::~AdbController() = default;

int AdbController::run_(const std::vector<std::string>& args,
                        const std::string& stdin_data,
                        std::string* out_stdout) {
    return spawn_capture(adb_path_, args, stdin_data, out_stdout);
}

bool AdbController::pair(const std::string& ip_port,
                         const std::string& code,
                         std::string* out_message) {
    std::string out;
    int rc = run_({"pair", ip_port}, code + "\n", &out);
    if (out_message) *out_message = out;
    return rc == 0 && out.find("Successfully paired") != std::string::npos;
}

bool AdbController::connect(const std::string& ip_port, std::string* out_message) {
    std::string out;
    int rc = run_({"connect", ip_port}, {}, &out);
    if (out_message) *out_message = out;
    return rc == 0 &&
           (out.find("connected") != std::string::npos ||
            out.find("already")   != std::string::npos);
}

bool AdbController::disconnect(const std::string& ip_port) {
    std::vector<std::string> args = {"disconnect"};
    if (!ip_port.empty()) args.push_back(ip_port);
    return run_(args, {}, nullptr) == 0;
}

std::vector<AdbController::MdnsService> AdbController::mdns_services() {
    std::vector<MdnsService> result;
    std::string out;
    if (run_({"mdns", "services"}, {}, &out) != 0) return result;

    // Output:
    //   List of discovered mdns services
    //   adb-XXXX-YYYY\t_adb-tls-connect._tcp.\t192.168.1.50:42745
    // Some adb builds use spaces instead of tabs.
    std::istringstream iss(out);
    std::string line;
    std::getline(iss, line); // header
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        MdnsService s;
        ls >> s.name >> s.type >> s.ip_port;
        if (s.ip_port.empty() || s.ip_port.find(':') == std::string::npos) continue;
        result.push_back(std::move(s));
    }
    return result;
}

std::string AdbController::mdns_check() {
    std::string out;
    run_({"mdns", "check"}, {}, &out);
    return out;
}

bool AdbController::kill_server() {
    return run_({"kill-server"}, {}, nullptr) == 0;
}

std::vector<DeviceInfo> AdbController::list_devices() {
    std::vector<DeviceInfo> result;
    std::string out;
    if (run_({"devices", "-l"}, {}, &out) != 0) return result;

    std::istringstream iss(out);
    std::string line;
    std::getline(iss, line); // header "List of devices attached"
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        DeviceInfo d;
        ls >> d.serial >> d.state;
        if (d.serial.empty()) continue;
        if (d.state == "offline" ||
            d.state.find("unauthorized") != std::string::npos) continue;
        auto mp = line.find("model:");
        if (mp != std::string::npos) {
            auto ep = line.find(' ', mp);
            d.model = line.substr(mp + 6,
                ep == std::string::npos ? std::string::npos : ep - (mp + 6));
        }
        result.push_back(std::move(d));
    }
    return result;
}

int AdbController::run_shell(const std::string& serial,
                              const std::string& cmd,
                              std::string* out_stdout) {
    return run_({"-s", serial, "shell", cmd}, {}, out_stdout);
}

bool AdbController::push(const std::string& serial,
                          const std::string& local,
                          const std::string& remote) {
    return run_({"-s", serial, "push", local, remote}, {}, nullptr) == 0;
}

bool AdbController::reverse(const std::string& serial,
                             const std::string& remote,
                             const std::string& local) {
    return run_({"-s", serial, "reverse", remote, local}, {}, nullptr) == 0;
}

bool AdbController::reverse_remove(const std::string& serial,
                                    const std::string& remote) {
    return run_({"-s", serial, "reverse", "--remove", remote}, {}, nullptr) == 0;
}

// ----------------------------------------------------------------------------
// ScrcpyReceiver
// ----------------------------------------------------------------------------

namespace {

bool read_exact(socket_t s, void* buf, size_t n) {
    auto* p = (char*)buf;
    while (n > 0) {
        int got = ::recv(s, p, (int)n, 0);
        if (got <= 0) return false;
        p += got; n -= (size_t)got;
    }
    return true;
}

uint32_t be32(const uint8_t* b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
           (uint32_t)b[2] << 8  | (uint32_t)b[3];
}
uint64_t be64(const uint8_t* b) {
    return ((uint64_t)be32(b) << 32) | (uint64_t)be32(b + 4);
}

constexpr uint64_t SCRCPY_PTS_FLAG_CONFIG = 1ULL << 63;
constexpr uint64_t SCRCPY_PTS_MASK         = (1ULL << 62) - 1;

constexpr uint32_t fourcc(const char (&s)[5]) {
    return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 |
           (uint32_t)(uint8_t)s[2] << 8  | (uint32_t)(uint8_t)s[3];
}

} // namespace

ScrcpyReceiver::ScrcpyReceiver() = default;

ScrcpyReceiver::~ScrcpyReceiver() {
    stop();
}

bool ScrcpyReceiver::start(const Config& cfg, AdbController& adb) {
    if (running_.load()) return true;
    cfg_ = cfg;
    adb_ = &adb;

    if (cfg.device_serial.empty() || cfg.server_jar_path.empty()) {
        std::cerr << "[scrcpy] missing device_serial or server_jar_path\n";
        return false;
    }

#ifdef _WIN32
    static std::once_flag wsa_once;
    std::call_once(wsa_once, [] {
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif

    // 1. Push the scrcpy server jar.
    std::cout << "[scrcpy] pushing " << cfg.server_jar_path << " ...\n";
    if (!adb.push(cfg.device_serial, cfg.server_jar_path,
                  "/data/local/tmp/scrcpy-server.jar")) {
        std::cerr << "[scrcpy] adb push failed\n";
        return false;
    }

    // 2. Open localhost listener BEFORE spawning the device-side server.
    socket_t listen_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCK) return false;
    int yes = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(cfg.local_port);
    if (::bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        ::listen(listen_sock, 1) != 0) {
        std::cerr << "[scrcpy] could not bind 127.0.0.1:" << cfg.local_port << "\n";
        CLOSESOCK(listen_sock);
        return false;
    }

    // 3. Reverse-tunnel device's localabstract:scrcpy → our local_port.
    adb.reverse_remove(cfg.device_serial, "localabstract:scrcpy");
    if (!adb.reverse(cfg.device_serial, "localabstract:scrcpy",
                     "tcp:" + std::to_string(cfg.local_port))) {
        std::cerr << "[scrcpy] adb reverse failed\n";
        CLOSESOCK(listen_sock);
        return false;
    }

    // 4. Spawn the scrcpy server. tunnel_forward=false → server connects
    //    out to the localabstract socket (which our reverse tunnel exposes).
    std::vector<std::string> srv_args = {
        "-s", cfg.device_serial,
        "shell",
        std::string("CLASSPATH=/data/local/tmp/scrcpy-server.jar ") +
        "app_process / com.genymobile.scrcpy.Server " + cfg.scrcpy_version +
        " video_codec=h264" +
        " audio=false" +
        " control=false" +
        " max_size=" + std::to_string(cfg.max_size) +
        " video_bit_rate=" + std::to_string(cfg.bit_rate) +
        " max_fps=" + std::to_string(cfg.max_fps) +
        " tunnel_forward=false" +
        " send_device_meta=true" +
        " send_codec_meta=true" +
        " send_frame_meta=true" +
        " cleanup=true"
    };
    std::cout << "[scrcpy] starting server v" << cfg.scrcpy_version
              << " on " << cfg.device_serial << "\n";

#ifdef _WIN32
    server_proc_ = spawn_detached(adb.adb_path(), srv_args);
    if (!server_proc_) {
        std::cerr << "[scrcpy] failed to spawn adb shell for server\n";
        CLOSESOCK(listen_sock);
        adb.reverse_remove(cfg.device_serial, "localabstract:scrcpy");
        return false;
    }
#endif

    // 5. Wait up to 10 s for the device callback.
    fd_set rfds; FD_ZERO(&rfds); FD_SET(listen_sock, &rfds);
    timeval tv{10, 0};
    int r = select((int)listen_sock + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0) {
        std::cerr << "[scrcpy] device did not connect within 10 s — "
                     "is wireless debugging on and the device unlocked?\n";
        CLOSESOCK(listen_sock);
        adb.reverse_remove(cfg.device_serial, "localabstract:scrcpy");
        return false;
    }
    socket_t s = ::accept(listen_sock, nullptr, nullptr);
    CLOSESOCK(listen_sock);
    if (s == INVALID_SOCK) {
        std::cerr << "[scrcpy] accept() failed\n";
        adb.reverse_remove(cfg.device_serial, "localabstract:scrcpy");
        return false;
    }
    sock_ = (uintptr_t)s;
    std::cout << "[scrcpy] device connected\n";

    running_.store(true);
    thread_ = std::thread([this] { worker_(); });
    return true;
}

void ScrcpyReceiver::stop() {
    bool was_running = running_.exchange(false);

    if (sock_ != (uintptr_t)INVALID_SOCK) {
        socket_t s = (socket_t)sock_;
#ifdef _WIN32
        shutdown(s, SD_BOTH);
#else
        shutdown(s, SHUT_RDWR);
#endif
    }
    if (was_running && thread_.joinable()) thread_.join();

    if (sock_ != (uintptr_t)INVALID_SOCK) {
        CLOSESOCK((socket_t)sock_);
        sock_ = (uintptr_t)INVALID_SOCK;
    }

#ifdef _WIN32
    if (server_proc_) {
        WaitForSingleObject((HANDLE)server_proc_, 1000);
        TerminateProcess((HANDLE)server_proc_, 0);
        CloseHandle((HANDLE)server_proc_);
        server_proc_ = nullptr;
    }
#endif

    if (adb_) adb_->reverse_remove(cfg_.device_serial, "localabstract:scrcpy");
    decoder_.reset();
}

void ScrcpyReceiver::worker_() {
    socket_t sock = (socket_t)sock_;

    // --- 1. device name (64 bytes, NUL-padded) ---
    uint8_t name_buf[64]{};
    if (!read_exact(sock, name_buf, sizeof(name_buf))) {
        std::cerr << "[scrcpy] header (device name) truncated\n";
        if (on_disconnect_) on_disconnect_();
        return;
    }
    device_name_.assign((const char*)name_buf,
                        strnlen((const char*)name_buf, sizeof(name_buf)));
    std::cout << "[scrcpy] device: " << device_name_ << "\n";

    // --- 2. codec metadata (12 bytes) ---
    uint8_t codec_meta[12];
    if (!read_exact(sock, codec_meta, sizeof(codec_meta))) {
        std::cerr << "[scrcpy] codec metadata truncated\n";
        if (on_disconnect_) on_disconnect_();
        return;
    }
    uint32_t codec_id = be32(codec_meta);
    video_w_ = (int)be32(codec_meta + 4);
    video_h_ = (int)be32(codec_meta + 8);
    std::cout << "[scrcpy] codec=0x" << std::hex << codec_id << std::dec
              << " " << video_w_ << "x" << video_h_ << "\n";

    int av_codec = AV_CODEC_ID_H264;
    if      (codec_id == fourcc("h264")) av_codec = AV_CODEC_ID_H264;
    else if (codec_id == fourcc("h265")) av_codec = AV_CODEC_ID_HEVC;
    else if (codec_id == fourcc("av01")) av_codec = AV_CODEC_ID_AV1;
    else std::cerr << "[scrcpy] unknown codec id; assuming h264\n";

    decoder_ = std::make_unique<media::Decoder>();
    if (!decoder_->init_video(av_codec)) {
        std::cerr << "[scrcpy] failed to init decoder\n";
        if (on_disconnect_) on_disconnect_();
        return;
    }
    decoder_->set_video_callback([this](media::VideoFrame f) {
        if (on_video_) on_video_(std::move(f));
    });

    // --- 3. frame loop ---
    std::vector<uint8_t> payload;
    payload.reserve(256 * 1024);

    while (running_.load()) {
        uint8_t hdr[12];
        if (!read_exact(sock, hdr, sizeof(hdr))) break;

        uint64_t pts_raw = be64(hdr);
        uint32_t size    = be32(hdr + 8);
        bool is_config   = (pts_raw & SCRCPY_PTS_FLAG_CONFIG) != 0;
        int64_t pts_us   = (int64_t)(pts_raw & SCRCPY_PTS_MASK);

        if (size == 0 || size > 16 * 1024 * 1024) {
            std::cerr << "[scrcpy] absurd frame size " << size << ", aborting\n";
            break;
        }

        payload.resize(size);
        if (!read_exact(sock, payload.data(), size)) break;

        decoder_->decode_video(payload.data(), payload.size(),
                               is_config ? 0 : pts_us);
    }

    std::cout << "[scrcpy] stream ended\n";
    if (on_disconnect_) on_disconnect_();
}

} // namespace opm::android

#endif // ENABLE_ANDROID
