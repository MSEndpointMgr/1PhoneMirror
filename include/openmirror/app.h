#pragma once

#include <openmirror/media/audio_output.h>
#include <openmirror/media/decoder.h>
#include <openmirror/media/renderer.h>
#include <atomic>
#include <memory>
#include <string>

#ifdef ENABLE_AIRPLAY
#include <openmirror/airplay/airplay_server.h>
#endif
#ifdef ENABLE_MIRACAST
#include <openmirror/miracast/miracast_receiver.h>
#endif
#ifdef ENABLE_CAST
#include <openmirror/cast/cast_receiver.h>
#endif

namespace openmirror {

class App {
public:
    App();
    ~App();

    struct Config {
        std::string name = "1PhoneMirror by MSEndpointMgr";
        int window_width = 1280;
        int window_height = 720;
        bool enable_airplay = true;
        bool enable_miracast = true;
        bool enable_cast = true;
    };

    bool init(const Config& config);

    // Run the main loop (blocks until quit)
    int run();

    void shutdown();

private:
    Config config_;

    media::Renderer renderer_;
    media::AudioOutput audio_;

    // Source priority: only one protocol streams at a time
    enum class Source { None, AirPlay, Miracast, Cast };
    std::atomic<int> active_source_{static_cast<int>(Source::None)};

#ifdef ENABLE_AIRPLAY
    airplay::AirPlayServer airplay_;
#endif
#ifdef ENABLE_MIRACAST
    miracast::MiracastReceiver miracast_;
#endif
#ifdef ENABLE_CAST
    cast::CastReceiver cast_;
#endif

    std::atomic<bool> running_{false};
};

} // namespace openmirror
