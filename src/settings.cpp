#include <openmirror/settings.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace openmirror {

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
            else if (k == "use_computer_name")            s.use_computer_name            = (v == "1" || v == "true");
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
    f << "use_computer_name="           << (use_computer_name           ? "1" : "0") << "\n";
    return true;
}

} // namespace openmirror
