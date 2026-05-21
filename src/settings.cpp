#include <opm/settings.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace opm {

static std::string settings_dir() {
#ifdef _WIN32
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        return std::string(appdata) + "\\1PhoneMirror";
    }
#endif
    return ".";
}

std::string Settings::file_path() {
    std::string dir = settings_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + "/settings.ini";
}

static std::string webcam_pending_path() {
    std::string dir = settings_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + "/webcam_pending.lock";
}

bool Settings::webcam_pending_exists() {
    std::error_code ec;
    return std::filesystem::exists(webcam_pending_path(), ec);
}

void Settings::set_webcam_pending() {
    std::ofstream f(webcam_pending_path(), std::ios::trunc);
    if (f.is_open()) f << "1";
}

void Settings::clear_webcam_pending() {
    std::error_code ec;
    std::filesystem::remove(webcam_pending_path(), ec);
}

static uint8_t clamp_mid_dark(int v) {
    // Restrict to a tasteful mid/dark range so the drawer/text contrast
    // remains readable. 18..160 keeps the bezel from going pure-black or
    // washed-out light.
    if (v < 18) v = 18;
    if (v > 160) v = 160;
    return (uint8_t)v;
}

Settings Settings::load() {
    Settings s;
    std::ifstream f(file_path());
    if (!f.is_open()) return s;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        // trim
        auto trim = [](std::string& x) {
            while (!x.empty() && (x.front() == ' ' || x.front() == '\t' || x.front() == '\r')) x.erase(x.begin());
            while (!x.empty() && (x.back()  == ' ' || x.back()  == '\t' || x.back()  == '\r')) x.pop_back();
        };
        trim(k); trim(v);
        try {
            if      (k == "bezel_r") s.bezel_r = clamp_mid_dark(std::stoi(v));
            else if (k == "bezel_g") s.bezel_g = clamp_mid_dark(std::stoi(v));
            else if (k == "bezel_b") s.bezel_b = clamp_mid_dark(std::stoi(v));
            else if (k == "screenshot_save_to_folder")   s.screenshot_save_to_folder   = (v == "1" || v == "true");
            else if (k == "screenshot_copy_to_clipboard") s.screenshot_copy_to_clipboard = (v == "1" || v == "true");
            else if (k == "screenshot_open_in_snagit")    s.screenshot_open_in_snagit    = (v == "1" || v == "true");
            else if (k == "use_computer_name")            s.use_computer_name            = (v == "1" || v == "true");
            else if (k == "telemetry_enabled")            s.telemetry_enabled            = (v == "1" || v == "true");
            else if (k == "always_on_top")                s.always_on_top                = (v == "1" || v == "true");
            else if (k == "record_format")                s.record_format                = std::clamp(std::stoi(v), 0, 1);
            else if (k == "record_max_duration_sec")      s.record_max_duration_sec      = std::max(0, std::stoi(v));
            else if (k == "record_fps_mp4")               s.record_fps_mp4               = std::clamp(std::stoi(v), 5, 60);
            else if (k == "record_fps_gif")               s.record_fps_gif               = std::clamp(std::stoi(v), 2, 30);
            else if (k == "record_bitrate_kbps")          s.record_bitrate_kbps          = std::clamp(std::stoi(v), 500, 50000);
            else if (k == "webcam_drawer_open")           s.webcam_drawer_open           = (v == "1" || v == "true");
            else if (k == "webcam_device_id")             s.webcam_device_id             = v;
            else if (k == "webcam_mirror_h")              s.webcam_mirror_h              = (v == "1" || v == "true");
            else if (k == "webcam_include_in_recording") s.webcam_include_in_recording = (v == "1" || v == "true");
        } catch (...) {}
    }
    return s;
}

bool Settings::save() const {
    std::ofstream f(file_path(), std::ios::trunc);
    if (!f.is_open()) return false;
    f << "bezel_r=" << (int)bezel_r << "\n";
    f << "bezel_g=" << (int)bezel_g << "\n";
    f << "bezel_b=" << (int)bezel_b << "\n";
    f << "screenshot_save_to_folder="   << (screenshot_save_to_folder   ? "1" : "0") << "\n";
    f << "screenshot_copy_to_clipboard=" << (screenshot_copy_to_clipboard ? "1" : "0") << "\n";
    f << "screenshot_open_in_snagit="  << (screenshot_open_in_snagit  ? "1" : "0") << "\n";
    f << "use_computer_name="           << (use_computer_name           ? "1" : "0") << "\n";
    f << "telemetry_enabled="           << (telemetry_enabled           ? "1" : "0") << "\n";
    f << "always_on_top="               << (always_on_top               ? "1" : "0") << "\n";
    f << "record_format="               << record_format << "\n";
    f << "record_max_duration_sec="     << record_max_duration_sec << "\n";
    f << "record_fps_mp4="              << record_fps_mp4 << "\n";
    f << "record_fps_gif="              << record_fps_gif << "\n";
    f << "record_bitrate_kbps="         << record_bitrate_kbps << "\n";
    f << "webcam_drawer_open="          << (webcam_drawer_open          ? "1" : "0") << "\n";
    f << "webcam_device_id="            << webcam_device_id << "\n";
    f << "webcam_mirror_h="             << (webcam_mirror_h             ? "1" : "0") << "\n";
    f << "webcam_include_in_recording=" << (webcam_include_in_recording ? "1" : "0") << "\n";
    return true;
}

} // namespace opm
