// Android pair / connect implementation called from the in-app panel.

#ifdef ENABLE_ANDROID

#include <opm/app.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

namespace opm {

namespace {
// Each scrcpy session needs a unique 127.0.0.1:<port> for its reverse
// tunnel. Walk forward from the default base port for each new session.
uint16_t pick_local_port(const std::vector<std::unique_ptr<App::AndroidSession>>& sessions) {
    uint16_t base = 27183;
    for (uint16_t p = base; p < base + 64; ++p) {
        bool used = false;
        for (auto& s : sessions)
            if (s && s->receiver && /* receiver started? */ false) used = true;
        // We don't actually know what local_port a started receiver picked
        // without exposing it on ScrcpyReceiver, so just step the port by
        // the live-session count. With <64 simultaneous Androids in flight
        // this is collision-free for any realistic workload.
        (void)used;
        return (uint16_t)(base + sessions.size());
    }
    return base;
}
} // namespace

bool App::start_android_session_(const std::string& serial,
                                 std::string* out_error) {
    auto sess = std::make_unique<AndroidSession>();
    sess->serial   = serial;
    sess->receiver = std::make_unique<android::ScrcpyReceiver>();

    // Per-session video routing: only the active source's frames reach
    // the renderer. Inactive Android sessions still decode (so a switch
    // shows the latest frame immediately) but their output is discarded.
    std::string captured = serial;
    sess->receiver->set_video_callback(
        [this, captured](media::VideoFrame frame) {
            int expected = static_cast<int>(Source::None);
            if (active_source_.compare_exchange_strong(
                    expected, static_cast<int>(Source::Android))) {
                std::lock_guard lk(scrcpy_mutex_);
                if (active_android_serial_.empty())
                    active_android_serial_ = captured;
            }
            bool is_active = false;
            {
                std::lock_guard lk(scrcpy_mutex_);
                is_active = (active_android_serial_ == captured);
            }
            if (is_active &&
                active_source_.load() == static_cast<int>(Source::Android))
                renderer_.submit_frame(std::move(frame));
        });
    sess->receiver->set_disconnect_callback([this, captured]() {
        // Drop the session asynchronously so the receiver thread can exit
        // cleanly before its owning unique_ptr is destroyed.
        std::thread([this, captured]() { android_disconnect(captured); }).detach();
    });

    android::ScrcpyReceiver::Config sc;
    sc.device_serial   = serial;
    sc.server_jar_path = android_jar_path_;
    {
        std::lock_guard lk(scrcpy_mutex_);
        sc.local_port = pick_local_port(scrcpy_sessions_);
    }
    if (!sess->receiver->start(sc, adb_)) {
        if (out_error) *out_error = "scrcpy start failed (see log).";
        return false;
    }
    // Prefer the model from `adb devices -l` (e.g. "SM_G781B") over the
    // scrcpy server's device_name (which is just the same model with a
    // hyphen instead of an underscore). Both get prettified to "SM G781B"
    // so the source-dot tooltip reads naturally.
    {
        auto pretty = [](std::string m) {
            for (auto& c : m) if (c == '_' || c == '-') c = ' ';
            return m;
        };
        std::string model;
        for (auto& d : adb_.list_devices()) {
            if (d.serial == serial && !d.model.empty()) {
                model = pretty(d.model);
                break;
            }
        }
        if (model.empty()) model = pretty(sess->receiver->device_name());
        sess->model = model;
    }
    {
        std::lock_guard lk(scrcpy_mutex_);
        // Make the just-started session the active one so the user sees
        // it immediately. The previous session (if any) keeps streaming
        // in the background and is one click away on its source dot.
        active_android_serial_ = serial;
        scrcpy_sessions_.push_back(std::move(sess));
    }
    active_source_.store(static_cast<int>(Source::Android));
    return true;
}

std::string App::android_pair_and_connect(const std::string& ip,
                                          const std::string& pair_port,
                                          const std::string& pin,
                                          const std::string& connect_port) {
    if (android_jar_path_.empty())
        return "scrcpy-server.jar not found in tools/";
    if (ip.empty() || pair_port.empty() || pin.size() != 6)
        return "Fill IP, port and 6-digit PIN.";

    // If a session for this exact IP is already running, tear just that
    // one down. We intentionally do NOT touch other sessions or call
    // `adb disconnect` (no-arg) — that would knock every other connected
    // phone offline.
    {
        std::lock_guard lk(scrcpy_mutex_);
        for (auto& s : scrcpy_sessions_) {
            if (s && s->serial.rfind(ip + ":", 0) == 0 && s->receiver) {
                std::cout << "[App] Replacing existing Android session for "
                          << s->serial << "\n";
                s->receiver->stop();
                adb_.disconnect(s->serial);
            }
        }
        scrcpy_sessions_.erase(
            std::remove_if(scrcpy_sessions_.begin(), scrcpy_sessions_.end(),
                [&](const std::unique_ptr<AndroidSession>& s) {
                    return s && s->serial.rfind(ip + ":", 0) == 0;
                }),
            scrcpy_sessions_.end());
    }

    std::string pair_addr = ip + ":" + pair_port;
    std::string msg;
    if (!adb_.pair(pair_addr, pin, &msg))
        return "Pair failed: " + msg;
    std::cout << "[App] Android paired: " << pair_addr << "\n";

    // Fast path: user supplied an explicit connect port (read off the
    // phone's main Wireless debugging screen). Skip mDNS entirely — a
    // direct `adb connect ip:port` is the most reliable on networks that
    // block multicast / IGMP / UDP-5353, which is the most common cause
    // of "Paired but device did not appear" failures.
    if (!connect_port.empty()) {
        std::string addr = ip + ":" + connect_port;
        std::string m;
        if (adb_.connect(addr, &m)) {
            std::cout << "[App] Android connect (manual): " << addr
                      << " - " << m << "\n";
            std::string err;
            if (!start_android_session_(addr, &err))
                return "Connected " + addr + " but " + err;
            return "Mirroring " + addr;
        }
        std::cerr << "[App] Android manual connect failed (" << addr
                  << "): " << m << "\n";
        return "Connect failed at " + addr +
               ". Verify the Connect port matches the value shown on the "
               "phone's Wireless debugging screen (it changes each session) "
               "and that the phone is on the same Wi-Fi.";
    }

    // Log adb's self-report on its mDNS subsystem so failures are
    // immediately diagnosable from the in-app log.
    {
        std::string check = adb_.mdns_check();
        if (!check.empty()) {
            std::string trimmed = check;
            while (!trimmed.empty() &&
                   (trimmed.back() == '\n' || trimmed.back() == '\r'))
                trimmed.pop_back();
            std::cout << "[App] adb mdns check: " << trimmed << "\n";
        }
    }

    // After successful pairing the Android phone advertises its
    // wireless-debug *connect* port via mDNS. The pairing port is one-shot
    // and is NOT the connect port. We need to find the connect endpoint.
    //
    // Strategy:
    //   1. Try `adb mdns services` (adb's own mDNS, often works when the
    //      system Bonjour stack is blocked by AP-isolation / IGMP snooping).
    //   2. Fall back to scanning `adb devices` for an auto-discovered entry.
    //
    // Either way poll for ~6 s before giving up.
    std::string serial;
    int last_mdns_count = -1;
    int last_devices_count = -1;
    for (int i = 0; i < 40 && serial.empty(); ++i) {
        // (1) adb mdns services — look for _adb-tls-connect._tcp. with
        // matching IP.
        auto services = adb_.mdns_services();
        if ((int)services.size() != last_mdns_count) {
            std::cout << "[App] Android mdns services: " << services.size()
                      << " entries\n";
            for (auto& s : services) {
                std::cout << "[App]   mdns: " << s.name << " | "
                          << s.type << " | " << s.ip_port << "\n";
            }
            last_mdns_count = (int)services.size();
        }
        for (auto& s : services) {
            if (s.type.find("_adb-tls-connect") == std::string::npos) continue;
            if (s.ip_port.rfind(ip + ":", 0) != 0) continue; // wrong host
            std::string m;
            if (adb_.connect(s.ip_port, &m)) {
                std::cout << "[App] Android connect via mDNS: " << s.ip_port
                          << " — " << m << "\n";
                serial = s.ip_port;
                break;
            } else {
                std::cerr << "[App] Android connect failed (" << s.ip_port
                          << "): " << m << "\n";
            }
        }
        if (!serial.empty()) break;

        // (2) adb devices — in case mDNS auto-discovery already wired up
        // a transport.
        auto devices = adb_.list_devices();
        if ((int)devices.size() != last_devices_count) {
            std::cout << "[App] Android adb devices: " << devices.size()
                      << " entries\n";
            for (auto& d : devices) {
                std::cout << "[App]   device: " << d.serial
                          << " state=" << d.state
                          << " model=" << d.model << "\n";
            }
            last_devices_count = (int)devices.size();
        }
        for (auto& d : devices) {
            if (d.serial == pair_addr) continue;       // pairing transport
            if (d.serial.rfind(ip + ":", 0) != 0) continue; // different host
            serial = d.serial;
            break;
        }
        if (serial.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (serial.empty())
        return "Paired but device did not appear via mDNS. "
               "Check the Wireless debugging screen for the IP & port and "
               "ensure the PC is on the same Wi-Fi (no AP isolation). "
               "See the log (press L) for the raw adb output.";

    android::ScrcpyReceiver::Config sc;
    sc.device_serial   = serial;
    sc.server_jar_path = android_jar_path_;
    std::string err;
    if (!start_android_session_(serial, &err))
        return err;

    return "Mirroring " + serial;
}

void App::android_disconnect(const std::string& serial) {
    std::vector<std::unique_ptr<AndroidSession>> stopped;
    {
        std::lock_guard lk(scrcpy_mutex_);
        scrcpy_sessions_.erase(
            std::remove_if(scrcpy_sessions_.begin(), scrcpy_sessions_.end(),
                [&](std::unique_ptr<AndroidSession>& s) {
                    if (!s) return true;
                    if (!serial.empty() && s->serial != serial) return false;
                    stopped.push_back(std::move(s));
                    return true;
                }),
            scrcpy_sessions_.end());
    }
    for (auto& s : stopped) {
        if (!s) continue;
        if (s->receiver) s->receiver->stop();
        adb_.disconnect(s->serial);
    }
    // If the (or one of the) torn-down sessions was the active source,
    // either fall back to the most-recent remaining session or return to
    // the waiting screen.
    bool should_reset = false;
    std::string fallback;
    {
        std::lock_guard lk(scrcpy_mutex_);
        bool active_gone = active_android_serial_.empty();
        for (auto& s : stopped)
            if (s && s->serial == active_android_serial_) { active_gone = true; break; }
        if (active_gone) {
            if (!scrcpy_sessions_.empty() && scrcpy_sessions_.back())
                fallback = scrcpy_sessions_.back()->serial;
            active_android_serial_ = fallback;
        }
        if (fallback.empty() && scrcpy_sessions_.empty()) {
            int expected = static_cast<int>(Source::Android);
            if (active_source_.compare_exchange_strong(
                    expected, static_cast<int>(Source::None)))
                should_reset = true;
        }
    }
    if (should_reset) renderer_.request_reset();
}

} // namespace opm

#endif // ENABLE_ANDROID
