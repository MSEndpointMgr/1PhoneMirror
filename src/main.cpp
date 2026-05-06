#include <openmirror/app.h>
#include <openmirror/config.h>
#include <openmirror/log_buffer.h>
#include <openmirror/network/tcp_server.h>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

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
              << "  --airplay-pin      Require on-screen PIN for AirPlay (managed iOS)\n"
              << "  --help             Show this help\n";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Install crash handler first so we capture failures during startup too.
    SetUnhandledExceptionFilter(om_unhandled_filter);
    // Initialize Winsock
    openmirror::network::TcpServer::init_winsock();
    // Check and offer to create firewall rules
    check_firewall_rules();
    // Install log capture (tees cout to internal buffer)
    openmirror::LogBuffer::instance().install();
#endif

    openmirror::App::Config config;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--name" && i + 1 < argc) {
            config.name = argv[++i];
        } else if (arg == "--width" && i + 1 < argc) {
            config.window_width = std::stoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            config.window_height = std::stoi(argv[++i]);
        } else if (arg == "--no-airplay") {
            config.enable_airplay = false;
        } else if (arg == "--no-miracast") {
            config.enable_miracast = false;
        } else if (arg == "--airplay-pin") {
            config.airplay_require_pin = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    openmirror::App app;

    if (!app.init(config)) {
        std::cerr << "Failed to initialize 1PhoneMirror\n";
        return 1;
    }

    int result = app.run();

    app.shutdown();

#ifdef _WIN32
    openmirror::network::TcpServer::cleanup_winsock();
#endif

    return result;
}
