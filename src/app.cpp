#include <openmirror/app.h>
#include <openmirror/config.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <tlhelp32.h>
#endif

namespace openmirror {

namespace {
#if defined(_WIN32) && defined(ENABLE_ANDROID)
// Forcibly terminate every adb.exe process (any path) on the system.
// `adb kill-server` is unreliable from a shutting-down process: it spawns
// another adb.exe to send the kill command, and if our process exits
// before that subprocess finishes, the daemon survives. We need adb gone
// because it binds UDP 5353 (mDNS) via its openscreen backend, which
// competes with Apple Bonjour and silently breaks AirPlay discovery on
// the next launch — the iPhone's spinning wheel never resolves until
// the user signs out/in of Windows.
static void kill_adb_processes() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int killed = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"adb.exe") != 0) continue;
            HANDLE hp = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,
                                    FALSE, pe.th32ProcessID);
            if (!hp) continue;
            if (TerminateProcess(hp, 1)) {
                WaitForSingleObject(hp, 1000);
                ++killed;
            }
            CloseHandle(hp);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (killed > 0) {
        std::cout << "[Shutdown] Terminated " << killed
                  << " adb.exe process(es) to release UDP 5353\n";
    }
}
#endif
#ifdef ENABLE_ANDROID
std::string resolve_path(const std::string& cfg, const char* env,
                         const std::string& exe_relative,
                         bool fallback_path_lookup) {
    if (!cfg.empty()) return cfg;
    if (const char* e = std::getenv(env); e && *e) return e;
    namespace fs = std::filesystem;
    std::error_code ec;
    auto exe_dir = fs::current_path(ec);
#ifdef _WIN32
    char buf[MAX_PATH]{};
    if (::GetModuleFileNameA(nullptr, buf, MAX_PATH))
        exe_dir = fs::path(buf).parent_path();
#endif
    auto candidate = exe_dir / exe_relative;
    if (fs::exists(candidate, ec)) return candidate.string();
    return fallback_path_lookup ? "adb" : std::string{};
}
#endif
} // namespace

App::App() = default;
App::~App() { shutdown(); }

bool App::init(const Config& config) {
    config_ = config;

    std::cout << "========================================\n";
    std::cout << "  " << OPENMIRROR_APP_NAME << " v"
              << OPENMIRROR_VERSION_MAJOR << "."
              << OPENMIRROR_VERSION_MINOR << "."
              << OPENMIRROR_VERSION_PATCH << "\n";
    std::cout << "========================================\n\n";

    // Initialize the renderer (SDL2 window)
    if (!renderer_.init(config_.name, config_.window_width, config_.window_height)) {
        std::cerr << "[App] Failed to initialize renderer\n";
        return false;
    }

    // Initialize audio output
    if (!audio_.init(44100, 2)) {
        std::cerr << "[App] Warning: audio output failed, continuing without audio\n";
    }

    // Lambda to route decoded frames to the renderer
    auto on_video = [this](media::VideoFrame frame) {
        renderer_.submit_frame(std::move(frame));
    };

    auto on_audio = [this](media::AudioFrame frame) {
        audio_.submit(std::move(frame));
    };

#ifdef ENABLE_AIRPLAY
    if (config_.enable_airplay) {
        airplay_.set_video_callback([this](media::VideoFrame frame) {
            int expected = static_cast<int>(Source::None);
            active_source_.compare_exchange_strong(expected, static_cast<int>(Source::AirPlay));
            if (active_source_.load() == static_cast<int>(Source::AirPlay))
                renderer_.submit_frame(std::move(frame));
        });
        airplay_.set_audio_callback([this](media::AudioFrame frame) {
            if (active_source_.load() == static_cast<int>(Source::AirPlay))
                audio_.submit(std::move(frame));
        });

        airplay::AirPlayServer::Config ap_config;
        ap_config.server_name = config_.name;
        ap_config.port = AIRPLAY_PORT;
        ap_config.mirror_port = AIRPLAY_MIRROR_PORT;

        airplay_.set_require_pin(config_.airplay_require_pin);
        airplay_.set_pin_display_callback([this](const std::string& pin) {
            renderer_.set_pin_code(pin);
        });

        // Multi-source picker wiring (AirPlay sources + Android, when active)
        renderer_.set_source_provider(
            [this]() {
                std::vector<media::Renderer::SourceEntry> out;
                const bool airplay_is_active =
                    (active_source_.load() == static_cast<int>(Source::AirPlay));
                for (auto& s : airplay_.list_sources())
                    out.push_back({s.id, s.name,
                                   airplay_is_active && s.active,
                                   s.streaming, s.paused});
#ifdef ENABLE_ANDROID
                {
                    std::lock_guard lk(scrcpy_mutex_);
                    for (auto& s : scrcpy_sessions_) {
                        if (!s || !s->receiver) continue;
                        media::Renderer::SourceEntry e;
                        e.id        = "android:" + s->serial;
                        e.name      = s->model.empty() ? std::string("Android phone")
                                                       : s->model;
                        e.active    = (active_source_.load() == static_cast<int>(Source::Android) &&
                                       active_android_serial_ == s->serial);
                        e.streaming = true;
                        out.push_back(std::move(e));
                    }
                }
#endif
                // Order by connection time: append new IDs in the order
                // they first appear, drop IDs that have gone away.
                std::lock_guard lk(source_order_mutex_);
                {
                    std::vector<std::string> live;
                    for (auto& e : out) live.push_back(e.id);
                    source_order_.erase(
                        std::remove_if(source_order_.begin(), source_order_.end(),
                            [&](const std::string& id) {
                                return std::find(live.begin(), live.end(), id) == live.end();
                            }),
                        source_order_.end());
                    for (auto& id : live)
                        if (std::find(source_order_.begin(), source_order_.end(), id) == source_order_.end())
                            source_order_.push_back(id);
                }
                std::sort(out.begin(), out.end(),
                    [this](const media::Renderer::SourceEntry& a,
                           const media::Renderer::SourceEntry& b) {
                        auto ai = std::find(source_order_.begin(), source_order_.end(), a.id);
                        auto bi = std::find(source_order_.begin(), source_order_.end(), b.id);
                        return ai < bi;
                    });
                return out;
            },
            [this](const std::string& id) {
#ifdef ENABLE_ANDROID
                if (id.rfind("android:", 0) == 0) {
                    {
                        std::lock_guard lk(scrcpy_mutex_);
                        active_android_serial_ = id.substr(8);
                    }
                    active_source_.store(static_cast<int>(Source::Android));
                    return;
                }
#endif
                active_source_.store(static_cast<int>(Source::AirPlay));
                airplay_.set_active_source(id);
            },
            [this](const std::string& id) {
                // Run disconnect off the UI thread — it can take seconds
                // (joins scrcpy worker threads, spawns `adb disconnect`,
                // closes RTSP sockets). Blocking the renderer caused
                // "Not Responding" hangs that left iOS unable to reconnect.
#ifdef ENABLE_ANDROID
                if (id.rfind("android:", 0) == 0) {
                    std::string serial = id.substr(8);
                    std::thread([this, serial]() { android_disconnect(serial); }).detach();
                    return;
                }
#endif
                std::thread([this, id]() { airplay_.disconnect_source(id); }).detach();
            });

        if (airplay_.start(ap_config)) {
            std::cout << "[App] AirPlay receiver active (iOS)\n";
        } else {
            std::cerr << "[App] Warning: AirPlay failed to start\n";
        }
        airplay_.set_disconnect_callback([this]() {
            int expected = static_cast<int>(Source::AirPlay);
            if (active_source_.compare_exchange_strong(expected, static_cast<int>(Source::None)))
                renderer_.request_reset();
        });
    }
#endif

#ifdef ENABLE_MIRACAST
    if (config_.enable_miracast) {
        miracast_.set_video_callback([this](media::VideoFrame frame) {
            int expected = static_cast<int>(Source::None);
            active_source_.compare_exchange_strong(expected, static_cast<int>(Source::Miracast));
            if (active_source_.load() == static_cast<int>(Source::Miracast))
                renderer_.submit_frame(std::move(frame));
        });
        miracast_.set_audio_callback([this](media::AudioFrame frame) {
            if (active_source_.load() == static_cast<int>(Source::Miracast))
                audio_.submit(std::move(frame));
        });

        miracast::MiracastReceiver::Config mc_config;
        mc_config.display_name = config_.name;

        if (miracast_.start(mc_config)) {
            std::cout << "[App] Miracast receiver active (Android)\n";
        } else {
            std::cerr << "[App] Warning: Miracast failed to start\n";
        }
        miracast_.set_disconnect_callback([this]() {
            int expected = static_cast<int>(Source::Miracast);
            if (active_source_.compare_exchange_strong(expected, static_cast<int>(Source::None)))
                renderer_.request_reset();
        });
    }
#endif

#ifdef ENABLE_CAST
    if (config_.enable_cast) {
        cast_.set_video_callback([this](media::VideoFrame frame) {
            int expected = static_cast<int>(Source::None);
            active_source_.compare_exchange_strong(expected, static_cast<int>(Source::Cast));
            if (active_source_.load() == static_cast<int>(Source::Cast))
                renderer_.submit_frame(std::move(frame));
        });
        cast_.set_audio_callback([this](media::AudioFrame frame) {
            if (active_source_.load() == static_cast<int>(Source::Cast))
                audio_.submit(std::move(frame));
        });

        cast::CastReceiver::Config cc_config;
        cc_config.device_name = config_.name;
        cc_config.port = CAST_PORT;

        if (cast_.start(cc_config)) {
            std::cout << "[App] Google Cast receiver active (Android)\n";
        } else {
            std::cerr << "[App] Warning: Cast receiver failed to start\n";
        }
        cast_.set_disconnect_callback([this]() {
            int expected = static_cast<int>(Source::Cast);
            if (active_source_.compare_exchange_strong(expected, static_cast<int>(Source::None)))
                renderer_.request_reset();
        });
    }
#endif

#ifdef ENABLE_ANDROID
    if (config_.enable_android) {
        // Resolve binaries.
        std::string adb_path = resolve_path(
            config_.android_adb_path, "OPENMIRROR_ADB",
            "tools/adb/adb.exe", /*fallback_path_lookup=*/true);
        std::string jar_path = resolve_path(
            config_.android_scrcpy_jar, "OPENMIRROR_SCRCPY_JAR",
            "tools/scrcpy-server.jar", /*fallback_path_lookup=*/false);

        adb_.set_adb_path(adb_path);
        android_jar_path_ = jar_path;
        std::cout << "[App] Android: adb=" << adb_path
                  << " jar=" << (jar_path.empty() ? "<missing>" : jar_path) << "\n";

        // Per-session video routing is set up in start_android_session_().

        // Renderer's in-app panel calls these.
        renderer_.set_add_android_callback([this]() {
            renderer_.show_android_panel();
        });
        renderer_.set_android_handlers(
            [this](const std::string& ip, const std::string& port,
                   const std::string& pin, const std::string& cport) {
                return android_pair_and_connect(ip, port, pin, cport);
            },
            [this]() {
                // Run on a background thread — android_disconnect() joins
                // the scrcpy worker and shells out to `adb disconnect`,
                // which can take a few seconds and would otherwise stall
                // the SDL event loop and look like an app freeze.
                std::thread([this]() { android_disconnect(); }).detach();
            });

        // Auto-discover Android devices on the LAN by polling adb's own
        // mDNS browser. This intentionally bypasses the IP that Samsung's
        // Wireless Debugging screen reports (which can be the CGNAT
        // 100.64.x.x mobile-data address) and shows the real Wi-Fi IP that
        // adb actually sees on the local subnet.
        renderer_.set_android_discover_callback(
            [this]() {
                std::vector<media::Renderer::DiscoveredAndroidDevice> out;
                auto services = adb_.mdns_services();
                // Also pull `adb devices -l` so we can attach a friendly
                // model name (e.g. "SM_G781B" → "Galaxy S20 FE 5G") to any
                // device that's currently connected via TCP/IP. Devices
                // discovered only via mDNS (not yet paired) won't have a
                // model and fall back to the serial-style mDNS name.
                auto known = adb_.list_devices();
                struct Acc {
                    std::string name;
                    std::string ip;
                    std::string connect_port;
                    std::string pair_port;
                };
                std::vector<Acc> acc;
                auto split_ip_port = [](const std::string& s,
                                        std::string& ip, std::string& port) {
                    auto pos = s.rfind(':');
                    if (pos == std::string::npos) { ip = s; port.clear(); }
                    else { ip = s.substr(0, pos); port = s.substr(pos + 1); }
                };
                auto pretty_name = [](std::string n) {
                    // Strip the "._adb-tls-{connect,pairing}._tcp." suffix.
                    auto p = n.find("._adb-tls-");
                    if (p != std::string::npos) n.resize(p);
                    // Strip the "adb-" prefix that adb prepends.
                    if (n.rfind("adb-", 0) == 0) n.erase(0, 4);
                    // Strip the trailing "-XXXXXX" instance hash.
                    auto dash = n.rfind('-');
                    if (dash != std::string::npos &&
                        n.size() - dash <= 8 && n.size() - dash >= 5) {
                        n.resize(dash);
                    }
                    return n;
                };
                auto prettify_model = [](std::string m) {
                    // adb returns model with underscores in place of spaces.
                    for (auto& c : m) if (c == '_') c = ' ';
                    return m;
                };
                for (const auto& s : services) {
                    std::string ip, port;
                    split_ip_port(s.ip_port, ip, port);
                    if (ip.empty()) continue;
                    auto it = std::find_if(acc.begin(), acc.end(),
                        [&](const Acc& a) { return a.ip == ip; });
                    if (it == acc.end()) {
                        acc.push_back({pretty_name(s.name), ip, {}, {}});
                        it = acc.end() - 1;
                    } else if (it->name.empty()) {
                        it->name = pretty_name(s.name);
                    }
                    if (s.type.find("pairing") != std::string::npos)
                        it->pair_port = port;
                    else
                        it->connect_port = port;
                }
                // Enrich with model info from `adb devices -l` when the
                // discovered ip:port matches an already-connected device.
                for (auto& a : acc) {
                    for (const auto& d : known) {
                        if (d.model.empty()) continue;
                        std::string dip, dport;
                        split_ip_port(d.serial, dip, dport);
                        if (dip == a.ip) {
                            a.name = prettify_model(d.model);
                            break;
                        }
                    }
                }
                // Also enrich from currently-active scrcpy sessions, which
                // already store a friendly model name. This covers the
                // case where a phone has just gone offline and adb's
                // device list dropped the entry, but the user reopens the
                // panel before our session noticed.
                {
                    std::lock_guard lk(scrcpy_mutex_);
                    for (auto& a : acc) {
                        if (!a.name.empty() && a.name.find(' ') != std::string::npos)
                            continue; // already a friendly name
                        for (auto& s : scrcpy_sessions_) {
                            if (!s) continue;
                            std::string sip, sport;
                            split_ip_port(s->serial, sip, sport);
                            if (sip == a.ip && !s->model.empty()) {
                                a.name = s->model;
                                break;
                            }
                        }
                    }
                }
                for (auto& a : acc) {
                    media::Renderer::DiscoveredAndroidDevice d;
                    d.label        = a.name;
                    d.ip           = a.ip;
                    d.connect_port = a.connect_port;
                    d.pair_port    = a.pair_port;
                    out.push_back(std::move(d));
                }
                return out;
            });

        // Re-register source provider so the Android dot also appears when
        // AirPlay is disabled (the AirPlay block registers a richer one).
        renderer_.set_source_provider(
            [this]() {
                std::vector<media::Renderer::SourceEntry> out;
#ifdef ENABLE_AIRPLAY
                const bool airplay_is_active =
                    (active_source_.load() == static_cast<int>(Source::AirPlay));
                for (auto& s : airplay_.list_sources())
                    out.push_back({s.id, s.name,
                                   airplay_is_active && s.active,
                                   s.streaming, s.paused});
#endif
                {
                    std::lock_guard lk(scrcpy_mutex_);
                    for (auto& s : scrcpy_sessions_) {
                        if (!s || !s->receiver) continue;
                        media::Renderer::SourceEntry e;
                        e.id        = "android:" + s->serial;
                        e.name      = s->model.empty() ? std::string("Android phone")
                                                       : s->model;
                        e.active    = (active_source_.load() == static_cast<int>(Source::Android) &&
                                       active_android_serial_ == s->serial);
                        e.streaming = true;
                        out.push_back(std::move(e));
                    }
                }
                std::lock_guard lk(source_order_mutex_);
                {
                    std::vector<std::string> live;
                    for (auto& e : out) live.push_back(e.id);
                    source_order_.erase(
                        std::remove_if(source_order_.begin(), source_order_.end(),
                            [&](const std::string& id) {
                                return std::find(live.begin(), live.end(), id) == live.end();
                            }),
                        source_order_.end());
                    for (auto& id : live)
                        if (std::find(source_order_.begin(), source_order_.end(), id) == source_order_.end())
                            source_order_.push_back(id);
                }
                std::sort(out.begin(), out.end(),
                    [this](const media::Renderer::SourceEntry& a,
                           const media::Renderer::SourceEntry& b) {
                        auto ai = std::find(source_order_.begin(), source_order_.end(), a.id);
                        auto bi = std::find(source_order_.begin(), source_order_.end(), b.id);
                        return ai < bi;
                    });
                return out;
            },
            [this](const std::string& id) {
                if (id.rfind("android:", 0) == 0) {
                    {
                        std::lock_guard lk(scrcpy_mutex_);
                        active_android_serial_ = id.substr(8);
                    }
                    active_source_.store(static_cast<int>(Source::Android));
                    return;
                }
#ifdef ENABLE_AIRPLAY
                active_source_.store(static_cast<int>(Source::AirPlay));
                airplay_.set_active_source(id);
#endif
            },
            [this](const std::string& id) {
                // Off-thread: see comment in the AirPlay branch above.
                if (id.rfind("android:", 0) == 0) {
                    std::string serial = id.substr(8);
                    std::thread([this, serial]() { android_disconnect(serial); }).detach();
                    return;
                }
#ifdef ENABLE_AIRPLAY
                std::thread([this, id]() { airplay_.disconnect_source(id); }).detach();
#endif
            });

        // If a serial was supplied via CLI, auto-start (legacy / power-user path).
        if (!config_.android_device_serial.empty() && !jar_path.empty()) {
            std::string err;
            if (start_android_session_(config_.android_device_serial, &err))
                std::cout << "[App] Android scrcpy receiver active (CLI device)\n";
            else
                std::cerr << "[App] CLI Android start failed: " << err << "\n";
        } else {
            std::cout << "[App] Android: press A in the app to pair / connect a phone.\n";
        }
    }
#endif

    std::cout << "\n[App] Ready. Waiting for connections...\n";
    std::cout << "  Press F for fullscreen, P to toggle phone frame,\n";
    std::cout << "  Ctrl+S to screenshot, ESC to quit.\n\n";

    // Silent on-launch update check. The renderer spawns its own worker
    // thread, queries the GitHub releases API, and shows a toast only when
    // a newer version exists or (manually invoked) when explicitly asked.
    // Network failures are swallowed silently per product spec.
    renderer_.check_for_update_async(false);

    running_.store(true);
    return true;
}

int App::run() {
    // The renderer runs the SDL event loop on the main thread
    renderer_.run();
    return 0;
}

void App::shutdown() {
    // Idempotent: main.cpp calls shutdown() explicitly and ~App() also
    // calls it, plus shutdown() may run on different paths (window close,
    // signal). Double-running SDL_Quit / closing already-freed sockets has
    // historically caused crashes after a session was disconnected.
    bool expected = false;
    if (!shutdown_done_.compare_exchange_strong(expected, true)) {
        return;
    }
    running_.store(false);

#if defined(_WIN32) && defined(ENABLE_ANDROID)
    // Kill adb FIRST, before any other teardown step. Three reasons:
    //   1. adb's openscreen mDNS holds UDP 5353 and we MUST release it
    //      so the iPhone can find our AirPlay service on the next launch.
    //   2. If we wait until after scrcpy_sessions_.clear() and that hangs,
    //      the watchdog terminates us before adb is killed and the daemon
    //      survives.
    //   3. Killing adb first makes scrcpy_sessions_.clear() faster because
    //      its blocking adb subprocesses now fail immediately instead of
    //      waiting for a server that is no longer responding.
    kill_adb_processes();
#endif

    // Watchdog: if any teardown step blocks (e.g. Bonjour's
    // DNSServiceRefDeallocate occasionally hangs on Windows when the
    // service is in a degraded state), force-exit the process rather
    // than leaving the user with a frozen window they can't close.
    // 5 s is generous for any healthy teardown.
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cerr << "[Shutdown] WATCHDOG: timed out, forcing exit\n";
        std::cerr.flush();
#if defined(_WIN32)
        TerminateProcess(GetCurrentProcess(), 0);
#else
        std::_Exit(0);
#endif
    }).detach();

#ifdef ENABLE_AIRPLAY
    airplay_.stop();
#endif
#ifdef ENABLE_MIRACAST
    miracast_.stop();
#endif
#ifdef ENABLE_CAST
    cast_.stop();
#endif
#ifdef ENABLE_ANDROID
    scrcpy_sessions_.clear();
#if defined(_WIN32)
    // Final sweep: scrcpy_sessions_.clear() above (or any late renderer
    // android-discovery callback) may have spawned a new adb.exe between
    // the early kill at the top of shutdown() and now. Kill them again so
    // we exit with UDP 5353 truly released.
    kill_adb_processes();
#endif
#endif

    audio_.shutdown();
    renderer_.shutdown();

    std::cout << "[App] Shutdown complete\n";
}

} // namespace openmirror
