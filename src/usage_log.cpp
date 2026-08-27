#include <opm/usage_log.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#endif

namespace opm {

namespace {

std::mutex g_session_mutex;
// Open sessions, keyed by "platform\x1Fdevice". A map (not two globals)
// so concurrent connections of the same platform (e.g. two iPhones, or two
// Android phones) each get their own start time instead of clobbering it.
std::map<std::string, std::chrono::steady_clock::time_point> g_open_sessions;

std::string session_key(const std::string& platform, const std::string& device) {
    return platform + "\x1F" + device;
}

std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local_tm);
    return buf;
}

// None of our fields contain commas/quotes today; escape defensively.
std::string csv_escape(const std::string& s) {
    if (s.find(',') == std::string::npos && s.find('"') == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += "\"\""; else out += c; }
    out += "\"";
    return out;
}

void append_row(const std::string& event, const std::string& platform,
                const std::string& device = "", long long duration_sec = -1) {
    std::string path = opm::UsageLog::file_path();
    bool need_header = !std::filesystem::exists(path);
    std::ofstream f(path, std::ios::app);
    if (!f.is_open()) return;
    if (need_header) f << "timestamp,event,platform,device,duration_sec\n";
    f << now_iso() << "," << event << "," << csv_escape(platform) << ","
      << csv_escape(device) << ","
      << (duration_sec >= 0 ? std::to_string(duration_sec) : std::string()) << "\n";
}

} // namespace

std::string UsageLog::file_path() {
    std::string dir = ".";
#ifdef _WIN32
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
        dir = std::string(appdata) + "\\1PhoneMirror";
#endif
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + "/usage_log.csv";
}

void UsageLog::log_app_start() { append_row("app_start", ""); }
void UsageLog::log_app_stop()  { append_row("app_stop", ""); }

void UsageLog::log_session_start(const std::string& platform, const std::string& device) {
    {
        std::lock_guard<std::mutex> lk(g_session_mutex);
        g_open_sessions[session_key(platform, device)] = std::chrono::steady_clock::now();
    }
    append_row("session_start", platform, device);
}

void UsageLog::log_session_end(const std::string& platform, const std::string& device) {
    long long dur = 0;
    {
        std::lock_guard<std::mutex> lk(g_session_mutex);
        auto it = g_open_sessions.find(session_key(platform, device));
        if (it != g_open_sessions.end()) {
            dur = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - it->second).count();
            g_open_sessions.erase(it);
        }
    }
    append_row("session_end", platform, device, dur);
}

void UsageLog::close_all_open_sessions() {
    std::vector<std::pair<std::string, std::string>> to_close; // (platform, device)
    {
        std::lock_guard<std::mutex> lk(g_session_mutex);
        for (auto& [key, start] : g_open_sessions) {
            auto sep = key.find('\x1F');
            if (sep == std::string::npos) continue;
            to_close.emplace_back(key.substr(0, sep), key.substr(sep + 1));
        }
    }
    for (auto& [platform, device] : to_close) log_session_end(platform, device);
}

void UsageLog::log_feature_use(const std::string& event, const std::string& platform) {
    append_row(event, platform);
}

std::vector<UsageLog::Row> UsageLog::load_rows() {
    std::vector<Row> rows;
    std::ifstream f(file_path());
    if (!f.is_open()) return rows;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) {
            first = false;
            if (line.rfind("timestamp,", 0) == 0) continue;
        }
        if (line.empty()) continue;
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) cols.push_back(cell);
        Row r;
        r.timestamp = cols.size() > 0 ? cols[0] : "";
        r.event     = cols.size() > 1 ? cols[1] : "";
        r.platform  = cols.size() > 2 ? cols[2] : "";
        // Rows written before the `device` column existed have 4 columns
        // (timestamp,event,platform,duration_sec); newer rows have 5.
        try {
            if (cols.size() >= 5) {
                r.device = cols[3];
                r.duration_sec = cols[4].empty() ? 0 : std::stoll(cols[4]);
            } else if (cols.size() == 4) {
                r.duration_sec = cols[3].empty() ? 0 : std::stoll(cols[3]);
            }
        } catch (...) {}
        rows.push_back(std::move(r));
    }
    return rows;
}

UsageLog::Stats UsageLog::compute_stats(const std::vector<Row>& rows) {
    Stats s;
    auto bump = [](ActionCounts& c, const std::string& platform) {
        if (platform == "iOS") c.ios++;
        else if (platform == "Android") c.android++;
    };
    for (auto& r : rows) {
        if (r.event == "app_start") {
            s.app_starts++;
        } else if (r.event == "session_start") {
            if (r.platform == "iOS") s.ios_sessions++;
            else if (r.platform == "Android") s.android_sessions++;
        } else if (r.event == "session_end") {
            if (r.platform == "iOS") s.ios_minutes += (double)r.duration_sec / 60.0;
            else if (r.platform == "Android") s.android_minutes += (double)r.duration_sec / 60.0;
        } else if (r.event == "screenshot") {
            bump(s.screenshots, r.platform);
        } else if (r.event == "annotate") {
            bump(s.annotations, r.platform);
        } else if (r.event == "ocr_copy") {
            bump(s.ocr_copies, r.platform);
        } else if (r.event == "recording") {
            bump(s.recordings, r.platform);
        }
    }
    return s;
}

} // namespace opm
