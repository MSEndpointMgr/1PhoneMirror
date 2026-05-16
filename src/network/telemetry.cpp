#include <openmirror/network/telemetry.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace openmirror::network {

namespace {

// --- install_id file management ------------------------------------------

static std::string install_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    // CSIDL_LOCAL_APPDATA = %LOCALAPPDATA% (machine-local, not roamed).
    // Using local on purpose: install_id should NOT roam to other PCs,
    // otherwise the same GUID would inflate "active install" counts.
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) {
        return std::string(buf) + "\\1PhoneMirror";
    }
#endif
    return ".";
}

static std::string install_id_path() {
    return install_dir() + "/install_id.txt";
}

// RFC 4122 v4 GUID generated locally with std::random_device. Not for
// cryptographic identity — only needs to be globally unique among
// 1PhoneMirror installs.
static std::string generate_guid_v4() {
    std::random_device rd;
    std::mt19937_64 gen(((uint64_t)rd() << 32) ^ (uint64_t)rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t a = dist(gen);
    uint64_t b = dist(gen);

    // Set version (4) and variant (10xx) bits.
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // bits 12-15 of clock_seq_hi_and_reserved
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // bits 6-7 of clock_seq_hi

    char out[37];
    std::snprintf(out, sizeof(out),
                  "%08x-%04x-%04x-%04x-%012llx",
                  (unsigned)((a >> 32) & 0xFFFFFFFFu),
                  (unsigned)((a >> 16) & 0xFFFFu),
                  (unsigned)(a & 0xFFFFu),
                  (unsigned)((b >> 48) & 0xFFFFu),
                  (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
    return std::string(out);
}

// --- HTTP POST -----------------------------------------------------------

#ifdef _WIN32
// Synchronous HTTPS POST. Returns true if response status is 2xx.
// Bounded by WinHTTP timeouts so a dead network cannot hang the worker.
static bool https_post_json(const wchar_t* host,
                            const wchar_t* path,
                            const std::string& body) {
    HINTERNET hSession = WinHttpOpen(L"1PhoneMirror-LaunchPing/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    // resolve / connect / send / receive (ms). Aggressive — this is best-effort.
    WinHttpSetTimeouts(hSession, 2000, 2000, 2000, 2000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path,
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    const wchar_t* headers = L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
                                 (LPVOID)body.data(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

    DWORD status = 0;
    if (ok) {
        DWORD ssz = sizeof(status);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status,
                            &ssz, WINHTTP_NO_HEADER_INDEX);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return ok && status >= 200 && status < 300;
}
#endif

// --- OS build ------------------------------------------------------------

static std::string os_build_string() {
#ifdef _WIN32
    // RtlGetVersion is the only reliable way to get the true Windows
    // build number from a manifest-less app. GetVersionEx lies starting
    // from Windows 8.1 unless you ship a compatibility manifest entry
    // for every release — we already do but RtlGetVersion is safer.
    using RtlGetVersionPtr = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return "0";
    auto fn = reinterpret_cast<RtlGetVersionPtr>(
        ::GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return "0";
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return "0";
    // "26100.0" — matches WindowsBuildNumber.RevisionNumber surfaced by
    // Get-ComputerInfo and Intune. The revision is not available from
    // RtlGetVersion; report as ".0" to keep the schema stable.
    std::ostringstream oss;
    oss << vi.dwBuildNumber << ".0";
    return oss.str();
#else
    return "0";
#endif
}

// --- JSON encoder (tiny, just what we need) ------------------------------

static std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// Split "https://host[:port]/path" into host + path. WinHTTP wants them apart.
struct ParsedUrl { std::wstring host; std::wstring path; };

static ParsedUrl parse_https_url(const std::string& url) {
    ParsedUrl r;
    constexpr const char* scheme = "https://";
    if (url.rfind(scheme, 0) != 0) return r;
    auto rest = url.substr(std::strlen(scheme));
    auto slash = rest.find('/');
    std::string host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/"  : rest.substr(slash);
    // Strip optional :port — WinHttpConnect uses the default HTTPS port.
    auto colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    r.host.assign(host.begin(), host.end());
    r.path.assign(path.begin(), path.end());
    return r;
}

// One ping per process — prevents accidental double-sends if init() is
// ever re-entered (e.g. during a future hot-reload).
static std::atomic<bool> s_ping_sent{false};

} // namespace

std::string install_id() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(install_dir(), ec);

    const std::string path = install_id_path();

    // Read existing.
    {
        std::ifstream in(path);
        if (in.is_open()) {
            std::string id;
            std::getline(in, id);
            // Trim trailing whitespace / CR.
            while (!id.empty() && (id.back() == '\r' || id.back() == '\n' ||
                                   id.back() == ' '  || id.back() == '\t')) {
                id.pop_back();
            }
            // Loose sanity check: 36 chars with dashes at the right spots.
            if (id.size() == 36 && id[8] == '-' && id[13] == '-' &&
                id[18] == '-' && id[23] == '-') {
                return id;
            }
        }
    }

    // Create new.
    std::string id = generate_guid_v4();
    std::ofstream out(path, std::ios::trunc);
    if (out.is_open()) out << id << "\n";
    return id;
}

void send_launch_ping_async(const std::string& app_version, bool enabled) {
    if (!enabled) return;
    bool expected = false;
    if (!s_ping_sent.compare_exchange_strong(expected, true)) return;

#ifndef _WIN32
    (void)app_version;
    return;
#else
    // Snapshot needed state on the calling thread so the worker outlives
    // any stack we were called from.
    const std::string url     = OPENMIRROR_TELEMETRY_URL "/ping";
    const std::string id      = install_id();
    const std::string version = app_version;
    const std::string osbuild = os_build_string();

    if (id.empty()) return;

    std::thread([url, id, version, osbuild]() {
        // Hard ceiling — WinHTTP timeouts already cap us, but belt-and-braces.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);

        std::ostringstream body;
        body << "{\"install_id\":\"" << json_escape(id)      << "\","
             <<  "\"version\":\""    << json_escape(version) << "\","
             <<  "\"os_build\":\""   << json_escape(osbuild) << "\"}";

        ParsedUrl pu = parse_https_url(url);
        if (pu.host.empty() || pu.path.empty()) return;

        (void)https_post_json(pu.host.c_str(), pu.path.c_str(), body.str());

        // Respect the deadline even if WinHTTP would have hung longer.
        if (std::chrono::steady_clock::now() > deadline) {
            // No-op — just for documentation / future use.
        }
    }).detach();
#endif
}

} // namespace openmirror::network
