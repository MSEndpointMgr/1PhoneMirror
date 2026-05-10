#pragma once

#include <cstdint>
#include <string>

namespace openmirror {

// User-configurable runtime settings. Persisted to
// %APPDATA%\1PhoneMirror\settings.ini on Windows.
struct Settings {
    // Phone bezel colour (mid/dark range — clamped on load).
    uint8_t bezel_r = 28;
    uint8_t bezel_g = 28;
    uint8_t bezel_b = 30;

    // Screenshot behaviour
    bool screenshot_save_to_folder = true;
    bool screenshot_copy_to_clipboard = true;

    // Recording behaviour. format: 0 = MP4 (H.264), 1 = GIF.
    int  record_format = 0;
    int  record_max_duration_sec = 60;  // 0 = unlimited
    int  record_fps_mp4 = 30;
    int  record_fps_gif = 10;
    int  record_bitrate_kbps = 6000;

    // When true, the AirPlay/Cast/Miracast service name advertises as
    // "1PhoneMirror by <COMPUTERNAME>" instead of
    // "1PhoneMirror by MSEndpointMgr". Useful when multiple instances run
    // on the same network. Applied at next launch.
    bool use_computer_name = false;

    // Returns the path to the settings file (creates the directory if needed).
    static std::string file_path();

    // Load from disk. Missing/invalid file returns defaults.
    static Settings load();

    // Save to disk. Returns true on success.
    bool save() const;
};

} // namespace openmirror
