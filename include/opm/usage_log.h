#pragma once

#include <string>
#include <vector>

namespace opm {

// Local-only CSV usage log — %APPDATA%\1PhoneMirror\usage_log.csv. Never
// transmitted anywhere; purely for the in-app Statistics drawer (T key).
// Rows: timestamp,event,platform,device,duration_sec
//   event    = "app_start" | "app_stop" | "session_start" | "session_end"
//              | "screenshot" | "annotate" | "ocr_copy" | "recording"
//   platform = "" for app_start/app_stop, else "iOS" | "Android"
//   device   = stable per-connection identifier (AirPlay source id/name,
//              Android serial+model) so concurrent/sequential connections
//              of the same platform are distinguishable. "" if not applicable.
//   duration_sec = filled in only on session_end
// NOTE: rows written before this field existed have 4 columns (no device);
// load_rows() understands both shapes.
class UsageLog {
public:
    static std::string file_path();

    static void log_app_start();
    static void log_app_stop();
    // platform must be "iOS" or "Android". device identifies the specific
    // connection (e.g. AirPlay source id, Android serial) so simultaneous
    // or sequential connections of the same platform are tracked as
    // separate sessions instead of being merged into one.
    static void log_session_start(const std::string& platform, const std::string& device = "");
    static void log_session_end(const std::string& platform, const std::string& device = "");
    // Force-ends any sessions still open (app quit mid-mirror) so the log
    // never has a dangling session_start with no matching session_end.
    static void close_all_open_sessions();
    // One-shot feature usage, tagged with whichever platform was mirroring
    // at the time (platform may be empty if none was active).
    // event: "screenshot" | "annotate" | "ocr_copy" | "recording".
    static void log_feature_use(const std::string& event, const std::string& platform);

    struct Row {
        std::string timestamp;
        std::string event;
        std::string platform;
        std::string device;
        long long   duration_sec = 0;
    };
    struct ActionCounts {
        int ios = 0;
        int android = 0;
    };
    struct Stats {
        int    app_starts       = 0;
        int    ios_sessions     = 0;
        int    android_sessions = 0;
        double ios_minutes      = 0.0;
        double android_minutes  = 0.0;
        ActionCounts screenshots;
        ActionCounts annotations;
        ActionCounts ocr_copies;
        ActionCounts recordings;
    };

    // Re-reads the whole (small, append-only) file. Fine to call whenever
    // the Statistics drawer is (re)opened.
    static std::vector<Row> load_rows();
    static Stats compute_stats(const std::vector<Row>& rows);
};

} // namespace opm
