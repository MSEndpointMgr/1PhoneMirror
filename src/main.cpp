#include <opm/app.h>
#include <opm/config.h>
#include <opm/log_buffer.h>
#include <opm/network/tcp_server.h>
#include <opm/airplay/srp_pin.h>
#include <opm/settings.h>
#ifdef ENABLE_ANDROID
#include <opm/android/scrcpy_receiver.h>
#endif
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")

// Detect and terminate any leftover 1PhoneMirror.exe process from a previous
// run that crashed without releasing its sockets or its Bonjour registration.
// Without this, the new instance fails to (re)bind ports 7000/7100 and the
// iPhone's AirPlay picker keeps showing the stale advertisement, so screen
// mirroring requests go to a dead receiver and silently time out.
static void kill_stale_instances() {
    DWORD self_pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int killed = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self_pid) continue;
            if (_wcsicmp(pe.szExeFile, L"1PhoneMirror.exe") != 0) continue;

            HANDLE hp = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,
                                    FALSE, pe.th32ProcessID);
            if (!hp) continue;
            std::cerr << "[Startup] Found stale 1PhoneMirror process pid="
                      << pe.th32ProcessID << ", terminating...\n";
            if (TerminateProcess(hp, 1)) {
                // Wait briefly for the OS to release sockets and the mDNS
                // registration; 1.5 s is enough in practice.
                WaitForSingleObject(hp, 1500);
                ++killed;
            }
            CloseHandle(hp);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (killed > 0) {
        // Give Winsock and Bonjour a moment to fully reclaim the ports
        // (TCP sockets in TIME_WAIT, mDNS goodbye packets, etc.).
        Sleep(800);
        std::cerr << "[Startup] Reclaimed resources from " << killed
                  << " stale instance(s)\n";
    }
}

// Write a minidump and log a one-line summary on any unhandled SEH crash so
// post-mortem analysis can pinpoint the failing thread/address. Dumps go to
// %LOCALAPPDATA%\1PhoneMirror\Crashes\crash_<pid>_<ts>.dmp .
static LONG WINAPI om_unhandled_filter(EXCEPTION_POINTERS* info) {
    if (info && info->ExceptionRecord) {
        std::cerr << "[CRASH] code=0x" << std::hex
                  << info->ExceptionRecord->ExceptionCode
                  << " addr=0x" << reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress)
                  << " thread=0x" << GetCurrentThreadId() << std::dec << "\n";
        std::cerr.flush();
    }

    char base[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH) == 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    char dir[MAX_PATH];
    std::snprintf(dir, sizeof(dir), "%s\\1PhoneMirror\\Crashes", base);
    CreateDirectoryA(base, nullptr);
    char parent[MAX_PATH];
    std::snprintf(parent, sizeof(parent), "%s\\1PhoneMirror", base);
    CreateDirectoryA(parent, nullptr);
    CreateDirectoryA(dir, nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path),
                  "%s\\crash_%lu_%04d%02d%02d_%02d%02d%02d.dmp",
                  dir, GetCurrentProcessId(),
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE hf = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithDataSegs |
                                              MiniDumpWithThreadInfo |
                                              MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          hf, type, info ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(hf);
        std::cerr << "[CRASH] minidump written: " << path << "\n";
        std::cerr.flush();
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Check if 1PhoneMirror firewall rules exist, offer to create them if not
static void check_firewall_rules() {
    // Query for our TCP rule using netsh
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    char cmd[] = "netsh advfirewall firewall show rule name=\"1PhoneMirror (TCP)\"";
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return; // Can't check, skip
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exit_code == 0) {
        // Rules already exist
        return;
    }

    int mbResult = MessageBoxA(nullptr,
        "No firewall rules found for 1PhoneMirror.\n\n"
        "AirPlay/Cast devices on the network won't be able to connect without them.\n\n"
        "Would you like to create firewall rules now? (requires admin)",
        "1PhoneMirror - Firewall Setup", MB_YESNO | MB_ICONQUESTION);

    if (mbResult != IDYES) {
        std::cout << "[Firewall] Skipped by user.\n";
        return;
    }

    // Build a combined command that creates both rules — program-level rule
    // covers all dynamic ports (event, timing) automatically
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

    std::string netsh_cmd =
        "/C netsh advfirewall firewall add rule name=\"1PhoneMirror (TCP)\" "
        "dir=in action=allow protocol=TCP localport=7000,7100 profile=any "
        "&& netsh advfirewall firewall add rule name=\"1PhoneMirror (UDP)\" "
        "dir=in action=allow protocol=UDP localport=5353,7010,7011 profile=any "
        "&& netsh advfirewall firewall add rule name=\"1PhoneMirror (App)\" "
        "dir=in action=allow program=\"" + std::string(exe_path) + "\" "
        "profile=any enable=yes";

    HINSTANCE result = ShellExecuteA(
        nullptr, "runas", "cmd.exe", netsh_cmd.c_str(), nullptr, SW_HIDE);

    if (reinterpret_cast<intptr_t>(result) > 32) {
        // Give the elevated process a moment to finish
        Sleep(2000);

        // Verify it worked
        PROCESS_INFORMATION pi2{};
        char verify[] = "netsh advfirewall firewall show rule name=\"1PhoneMirror (TCP)\"";
        if (CreateProcessA(nullptr, verify, nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi2)) {
            WaitForSingleObject(pi2.hProcess, 5000);
            DWORD verify_code = 0;
            GetExitCodeProcess(pi2.hProcess, &verify_code);
            CloseHandle(pi2.hProcess);
            CloseHandle(pi2.hThread);
            if (verify_code == 0) {
                std::cout << "[Firewall] Rules created successfully.\n\n";
            } else {
                std::cout << "[Firewall] Could not verify rules. You may need to add them manually.\n\n";
            }
        }
    } else {
        std::cout << "[Firewall] Admin access denied. You can add rules manually:\n"
                  << "  netsh advfirewall firewall add rule name=\"1PhoneMirror (TCP)\" "
                  << "dir=in action=allow protocol=TCP localport=7000,7100 profile=any\n"
                  << "  netsh advfirewall firewall add rule name=\"1PhoneMirror (UDP)\" "
                  << "dir=in action=allow protocol=UDP localport=5353,7010,7011 profile=any\n\n";
    }
}
#endif

void print_usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n\n"
              << "Options:\n"
              << "  --name <name>      Display name (default: 1PhoneMirror)\n"
              << "  --width <pixels>   Window width (default: 1280)\n"
              << "  --height <pixels>  Window height (default: 720)\n"
              << "  --no-airplay       Disable AirPlay (iOS) receiver\n"
              << "  --no-miracast      Disable Miracast (Android) receiver\n"
              << "  --no-android       Disable Android (scrcpy) receiver\n"
              << "  --airplay-pin      Require on-screen PIN for AirPlay (managed iOS)\n"
              << "\nAndroid (scrcpy) options:\n"
              << "  --android-pair <ip:port> <code>   One-time pair via Wireless Debugging\n"
              << "  --android-connect <ip:port>       Connect to a paired device, then exit\n"
              << "  --android-device <serial>         Force a specific device serial\n"
              << "  --android-jar <path>              Path to scrcpy-server.jar\n"
              << "  --android-adb <path>              Path to adb.exe\n"
              << "  --help             Show this help\n";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Install crash handler first so we capture failures during startup too.
    SetUnhandledExceptionFilter(om_unhandled_filter);
    // Reap any leftover 1PhoneMirror.exe (from a previous crash) before we
    // try to bind ports or register the mDNS service.
    kill_stale_instances();
    // Initialize Winsock
    opm::network::TcpServer::init_winsock();
    // Check and offer to create firewall rules
    check_firewall_rules();
    // Install log capture (tees cout to internal buffer). File logging
    // is opt-in via the Settings panel toggle (saved alongside the user's
    // screenshots only for the current session).
    opm::LogBuffer::instance().install();
#endif

    opm::App::Config config;

    // Parse command line arguments
    bool name_overridden = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--name" && i + 1 < argc) {
            config.name = argv[++i];
            name_overridden = true;
        } else if (arg == "--width" && i + 1 < argc) {
            config.window_width = std::stoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            config.window_height = std::stoi(argv[++i]);
        } else if (arg == "--no-airplay") {
            config.enable_airplay = false;
        } else if (arg == "--no-miracast") {
            config.enable_miracast = false;
        } else if (arg == "--no-android") {
            config.enable_android = false;
        } else if (arg == "--android-device" && i + 1 < argc) {
            config.android_device_serial = argv[++i];
        } else if (arg == "--android-jar" && i + 1 < argc) {
            config.android_scrcpy_jar = argv[++i];
        } else if (arg == "--android-adb" && i + 1 < argc) {
            config.android_adb_path = argv[++i];
#ifdef ENABLE_ANDROID
        } else if (arg == "--android-pair" && i + 2 < argc) {
            std::string ipport = argv[++i];
            std::string code   = argv[++i];
            opm::android::AdbController adb;
            if (!config.android_adb_path.empty()) adb.set_adb_path(config.android_adb_path);
            std::string msg;
            bool ok = adb.pair(ipport, code, &msg);
            std::cout << msg;
            return ok ? 0 : 2;
        } else if (arg == "--android-connect" && i + 1 < argc) {
            std::string ipport = argv[++i];
            opm::android::AdbController adb;
            if (!config.android_adb_path.empty()) adb.set_adb_path(config.android_adb_path);
            std::string msg;
            bool ok = adb.connect(ipport, &msg);
            std::cout << msg;
            return ok ? 0 : 2;
#endif
        } else if (arg == "--airplay-pin") {
            config.airplay_require_pin = true;
        } else if (arg == "--srp-self-test") {
            bool ok = opm::airplay::srp_pin_self_test();
            return ok ? 0 : 2;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    opm::App app;

    // If the user enabled "Identify as <ComputerName>" in the Settings panel
    // and didn't pass an explicit --name, build the service name from the
    // local computer name. Helps disambiguate multiple instances on the same
    // network. Setting takes effect at next launch.
    if (!name_overridden) {
        auto s = opm::Settings::load();
        if (s.use_computer_name) {
#ifdef _WIN32
            char buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD len = sizeof(buf);
            if (GetComputerNameA(buf, &len) && len > 0) {
                config.name = std::string("1PhoneMirror by ") + buf;
            }
#endif
        }
    }

    if (!app.init(config)) {
        std::cerr << "Failed to initialize 1PhoneMirror\n";
        return 1;
    }

    int result = app.run();

    app.shutdown();

#ifdef _WIN32
    opm::network::TcpServer::cleanup_winsock();
#endif

    return result;
}
