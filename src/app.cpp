#include <openmirror/app.h>
#include <openmirror/config.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#if defined(ENABLE_ANDROID) && defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

namespace openmirror {

namespace {
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
                for (auto& s : airplay_.list_sources())
                    out.push_back({s.id, s.name, s.active, s.streaming});
#ifdef ENABLE_ANDROID
                if (scrcpy_.is_running()) {
                    media::Renderer::SourceEntry e;
                    e.id        = "android";
                    e.name      = "Android phone";
                    e.active    = (active_source_.load() == static_cast<int>(Source::Android));
                    e.streaming = true;
                    out.push_back(std::move(e));
                }
#endif
                return out;
            },
            [this](const std::string& id) {
#ifdef ENABLE_ANDROID
                if (id == "android") {
                    int expected = static_cast<int>(Source::None);
                    active_source_.compare_exchange_strong(
                        expected, static_cast<int>(Source::Android));
                    return;
                }
#endif
                airplay_.set_active_source(id);
            },
            [this](const std::string& id) {
#ifdef ENABLE_ANDROID
                if (id == "android") { android_disconnect(); return; }
#endif
                airplay_.disconnect_source(id);
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

        scrcpy_.set_video_callback([this](media::VideoFrame frame) {
            int expected = static_cast<int>(Source::None);
            active_source_.compare_exchange_strong(expected, static_cast<int>(Source::Android));
            if (active_source_.load() == static_cast<int>(Source::Android))
                renderer_.submit_frame(std::move(frame));
        });
        scrcpy_.set_disconnect_callback([this]() {
            int expected = static_cast<int>(Source::Android);
            if (active_source_.compare_exchange_strong(expected, static_cast<int>(Source::None)))
                renderer_.request_reset();
        });

        // Renderer's in-app panel calls these.
        renderer_.set_add_android_callback([this]() {
            renderer_.show_android_panel();
        });
        renderer_.set_android_handlers(
            [this](const std::string& ip, const std::string& port, const std::string& pin) {
                return android_pair_and_connect(ip, port, pin);
            },
            [this]() { android_disconnect(); });

        // Re-register source provider so the Android dot also appears when
        // AirPlay is disabled (the AirPlay block registers a richer one).
        renderer_.set_source_provider(
            [this]() {
                std::vector<media::Renderer::SourceEntry> out;
#ifdef ENABLE_AIRPLAY
                for (auto& s : airplay_.list_sources())
                    out.push_back({s.id, s.name, s.active, s.streaming});
#endif
                if (scrcpy_.is_running()) {
                    media::Renderer::SourceEntry e;
                    e.id        = "android";
                    e.name      = "Android phone";
                    e.active    = (active_source_.load() == static_cast<int>(Source::Android));
                    e.streaming = true;
                    out.push_back(std::move(e));
                }
                return out;
            },
            [this](const std::string& id) {
                if (id == "android") {
                    int expected = static_cast<int>(Source::None);
                    active_source_.compare_exchange_strong(
                        expected, static_cast<int>(Source::Android));
                    return;
                }
#ifdef ENABLE_AIRPLAY
                airplay_.set_active_source(id);
#endif
            },
            [this](const std::string& id) {
                if (id == "android") { android_disconnect(); return; }
#ifdef ENABLE_AIRPLAY
                airplay_.disconnect_source(id);
#endif
            });

        // If a serial was supplied via CLI, auto-start (legacy / power-user path).
        if (!config_.android_device_serial.empty() && !jar_path.empty()) {
            android::ScrcpyReceiver::Config sc;
            sc.device_serial    = config_.android_device_serial;
            sc.server_jar_path  = jar_path;
            if (scrcpy_.start(sc, adb_))
                std::cout << "[App] Android scrcpy receiver active (CLI device)\n";
        } else {
            std::cout << "[App] Android: press A in the app to pair / connect a phone.\n";
        }
    }
#endif

    std::cout << "\n[App] Ready. Waiting for connections...\n";
    std::cout << "  Press F for fullscreen, P to toggle phone frame,\n";
    std::cout << "  Ctrl+S to screenshot, ESC to quit.\n\n";

    running_.store(true);
    return true;
}

int App::run() {
    // The renderer runs the SDL event loop on the main thread
    renderer_.run();
    return 0;
}

void App::shutdown() {
    running_.store(false);

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
    scrcpy_.stop();
#endif

    audio_.shutdown();
    renderer_.shutdown();

    std::cout << "[App] Shutdown complete\n";
}

} // namespace openmirror
