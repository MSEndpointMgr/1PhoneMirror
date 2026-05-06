#pragma once

#ifdef ENABLE_ANDROID

#include <openmirror/media/decoder.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace openmirror::android {

// Wireless Android screen-mirror receiver based on the scrcpy server
// (Apache-2.0, https://github.com/Genymobile/scrcpy).
//
// Setup on the phone (one time):
//   Settings -> Developer options -> Wireless debugging -> ON
//   Tap "Pair device with pairing code" — phone shows IP:PORT + 6-digit code
//
// Setup on the PC (one time per phone):
//   AdbController::pair(ip_port, code)        — registers RSA trust
//
// Every session:
//   AdbController::connect(ip:5555)           — opens ADB transport
//   ScrcpyReceiver::start({ device_serial })  — pushes JAR, runs server,
//                                                streams H.264 NALs into the
//                                                supplied media::Decoder.
//
// No app is ever installed on the phone. The 70 KB scrcpy-server.jar lives
// only in /data/local/tmp on the device and is wiped on reboot.

struct DeviceInfo {
    std::string serial;          // ADB serial, e.g. "192.168.1.42:5555"
    std::string state;           // "device", "offline", "unauthorized", ...
    std::string model;           // ro.product.model (filled on demand)
    std::string android_version; // ro.build.version.release
};

class AdbController {
public:
    AdbController();
    ~AdbController();

    // Path to adb.exe. Defaults to "adb" (must be on PATH); the installer is
    // expected to ship Android Platform-Tools next to 1PhoneMirror.exe.
    void set_adb_path(const std::string& path) { adb_path_ = path; }
    const std::string& adb_path() const { return adb_path_; }

    // Run `adb pair <ip:port>` and pipe the supplied 6-digit code in.
    // Returns true if the device reported successful pairing.
    bool pair(const std::string& ip_port, const std::string& code,
              std::string* out_message = nullptr);

    // `adb connect <ip:port>` — opens or reuses a TCP transport.
    bool connect(const std::string& ip_port, std::string* out_message = nullptr);

    // `adb disconnect [<ip:port>]`.
    bool disconnect(const std::string& ip_port = {});

    // `adb devices -l`, parsed.
    std::vector<DeviceInfo> list_devices();

    // `adb -s <serial> shell <cmd>` — captures stdout/stderr.
    int run_shell(const std::string& serial, const std::string& cmd,
                  std::string* out_stdout = nullptr);

    // `adb -s <serial> push <local> <remote>`.
    bool push(const std::string& serial,
              const std::string& local, const std::string& remote);

    // `adb -s <serial> reverse <remote> <local>`.
    bool reverse(const std::string& serial,
                 const std::string& remote, const std::string& local);
    bool reverse_remove(const std::string& serial, const std::string& remote);

private:
    std::string adb_path_ = "adb";

    // Synchronously run an adb command, optionally feeding stdin and
    // capturing stdout. Returns the process exit code (or -1 on spawn fail).
    int run_(const std::vector<std::string>& args,
             const std::string& stdin_data,
             std::string* out_stdout);
};

class ScrcpyReceiver {
public:
    ScrcpyReceiver();
    ~ScrcpyReceiver();

    struct Config {
        std::string device_serial;            // e.g. "192.168.1.42:5555"
        std::string server_jar_path;          // local path to scrcpy-server.jar
        std::string scrcpy_version = "3.0";   // must match the JAR version
        uint16_t local_port = 27183;          // host side of the reverse tunnel
        int max_size = 1920;                  // resize-to-fit-square pixels
        int bit_rate = 8'000'000;             // bits/s (H.264)
        int max_fps = 60;
    };

    // Spawns the scrcpy server on the device, opens the reverse tunnel,
    // accepts the device's callback connection, and starts pumping H.264
    // NAL units into the supplied callback.
    bool start(const Config& cfg, AdbController& adb);
    void stop();

    bool is_running() const { return running_.load(); }

    void set_video_callback(media::OnVideoFrame cb) { on_video_ = std::move(cb); }
    void set_disconnect_callback(std::function<void()> cb) { on_disconnect_ = std::move(cb); }

    // After start() succeeds, these are populated from the device metadata
    // header sent by the scrcpy server.
    const std::string& device_name() const { return device_name_; }
    int video_width() const  { return video_w_; }
    int video_height() const { return video_h_; }

private:
    void worker_();

    Config cfg_;
    AdbController* adb_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::string device_name_;
    int video_w_ = 0;
    int video_h_ = 0;

    std::unique_ptr<media::Decoder> decoder_;

    media::OnVideoFrame on_video_;
    std::function<void()> on_disconnect_;

    // Opaque handles — kept as void*/uintptr_t so this header avoids
    // dragging in <winsock2.h> / <windows.h> for every translation unit.
    uintptr_t sock_ = (uintptr_t)-1;   // SOCKET / int
    void*     server_proc_ = nullptr;  // HANDLE on Windows
};

} // namespace openmirror::android

#endif // ENABLE_ANDROID
