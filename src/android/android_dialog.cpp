// Android pair / connect implementation called from the in-app panel.

#ifdef ENABLE_ANDROID

#include <openmirror/app.h>

#include <chrono>
#include <iostream>
#include <thread>

namespace openmirror {

std::string App::android_pair_and_connect(const std::string& ip,
                                          const std::string& pair_port,
                                          const std::string& pin) {
    if (android_jar_path_.empty())
        return "scrcpy-server.jar not found in tools/";
    if (ip.empty() || pair_port.empty() || pin.size() != 6)
        return "Fill IP, port and 6-digit PIN.";

    // Stop any active session and drop transports for a clean slate.
    scrcpy_.stop();
    adb_.disconnect();

    std::string pair_addr = ip + ":" + pair_port;
    std::string msg;
    if (!adb_.pair(pair_addr, pin, &msg))
        return "Pair failed: " + msg;
    std::cout << "[App] Android paired: " << pair_addr << "\n";

    // After successful pairing the Android phone announces itself via
    // mDNS and adb auto-discovers it within ~1-2 s. Poll briefly.
    std::string serial;
    for (int i = 0; i < 20 && serial.empty(); ++i) {
        for (auto& d : adb_.list_devices()) {
            // Skip the pairing transport (uses pair_port); we want the
            // mDNS-discovered device at its main wireless-debugging port.
            if (d.serial == pair_addr) continue;
            serial = d.serial;
            break;
        }
        if (serial.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (serial.empty())
        return "Paired but device did not appear via mDNS.";

    android::ScrcpyReceiver::Config sc;
    sc.device_serial   = serial;
    sc.server_jar_path = android_jar_path_;
    if (!scrcpy_.start(sc, adb_))
        return "scrcpy start failed (see log).";

    return "Mirroring " + serial;
}

void App::android_disconnect() {
    scrcpy_.stop();
    int expected = static_cast<int>(Source::Android);
    if (active_source_.compare_exchange_strong(expected, static_cast<int>(Source::None)))
        renderer_.request_reset();
}

} // namespace openmirror

#endif // ENABLE_ANDROID
