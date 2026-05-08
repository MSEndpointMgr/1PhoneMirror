#pragma once

#include <openmirror/media/audio_output.h>
#include <openmirror/media/decoder.h>
#include <openmirror/media/renderer.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef ENABLE_AIRPLAY
#include <openmirror/airplay/airplay_server.h>
#endif
#ifdef ENABLE_MIRACAST
#include <openmirror/miracast/miracast_receiver.h>
#endif
#ifdef ENABLE_CAST
#include <openmirror/cast/cast_receiver.h>
#endif
#ifdef ENABLE_ANDROID
#include <openmirror/android/scrcpy_receiver.h>
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
        bool enable_android = true;
        bool airplay_require_pin = false;

        // Android (scrcpy) — auto-detected at startup if these are set.
        std::string android_adb_path;          // path to adb.exe (empty = PATH)
        std::string android_scrcpy_jar;        // path to scrcpy-server.jar
        std::string android_device_serial;     // empty = auto-pick first
    };

    bool init(const Config& config);

    // Run the main loop (blocks until quit)
    int run();

    void shutdown();

#ifdef ENABLE_ANDROID
    // Pair via (ip, pair_port, pin) and start mirroring once the device
    // appears via mDNS auto-discovery. If `connect_port` is non-empty,
    // skips mDNS and connects directly to <ip>:<connect_port> (the value
    // shown on the phone's main Wireless debugging screen).
    // Returns a human-readable status.
    std::string android_pair_and_connect(const std::string& ip,
                                         const std::string& pair_port,
                                         const std::string& pin,
                                         const std::string& connect_port = "");
    void android_disconnect();
#endif

private:
    Config config_;

    media::Renderer renderer_;
    media::AudioOutput audio_;

    // Source priority: only one protocol streams at a time
    enum class Source { None, AirPlay, Miracast, Cast, Android };
    std::atomic<int> active_source_{static_cast<int>(Source::None)};

    // Connection order across kinds. Each entry is either an AirPlay
    // device id (e.g. "192.168.10.5") or the literal "android".
    // Used to render the bottom-bezel source dots in the order devices
    // actually connected.
    std::mutex                source_order_mutex_;
    std::vector<std::string>  source_order_;

#ifdef ENABLE_AIRPLAY
    airplay::AirPlayServer airplay_;
#endif
#ifdef ENABLE_MIRACAST
    miracast::MiracastReceiver miracast_;
#endif
#ifdef ENABLE_CAST
    cast::CastReceiver cast_;
#endif
#ifdef ENABLE_ANDROID
    android::AdbController   adb_;
    android::ScrcpyReceiver  scrcpy_;
    std::string android_jar_path_;
#endif

    std::atomic<bool> running_{false};
};

} // namespace openmirror
