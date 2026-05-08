// Android pair / connect implementation called from the in-app panel.

#ifdef ENABLE_ANDROID

#include <openmirror/app.h>

#include <chrono>
#include <iostream>
#include <thread>

namespace openmirror {

std::string App::android_pair_and_connect(const std::string& ip,
                                          const std::string& pair_port,
                                          const std::string& pin,
                                          const std::string& connect_port) {
    if (android_jar_path_.empty())
        return "scrcpy-server.jar not found in tools/";
    if (ip.empty() || pair_port.empty() || pin.size() != 6)
        return "Fill IP, port and 6-digit PIN.";

    // Stop any active session and drop transports for a clean slate.
    scrcpy_.stop();
    adb_.disconnect();

    // Recycle the adb server. Two reasons:
    //   1. Picks up the ADB_MDNS_OPENSCREEN=1 env var if a previous server
    //      was running with the (often stale) Bonjour backend.
    //   2. Forces a fresh mDNS browse — the Bonjour browse channel can go
    //      silent after sleep/wake or a network change, leading to the
    //      classic "Paired but device did not appear" failure even when
    //      the phone is perfectly reachable.
    adb_.kill_server();

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
            android::ScrcpyReceiver::Config sc;
            sc.device_serial   = addr;
            sc.server_jar_path = android_jar_path_;
            if (!scrcpy_.start(sc, adb_))
                return "Connected " + addr + " but scrcpy start failed (see log).";
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
