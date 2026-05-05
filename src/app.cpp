#include <openmirror/app.h>
#include <openmirror/config.h>
#include <iostream>

namespace openmirror {

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

    audio_.shutdown();
    renderer_.shutdown();

    std::cout << "[App] Shutdown complete\n";
}

} // namespace openmirror
