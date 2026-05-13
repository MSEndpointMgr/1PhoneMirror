#include <openmirror/network/update_check.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace openmirror::network {

namespace {

// Split "0.3.4" / "v0.3.4" into integer triplet. Anything missing -> 0.
struct SemVer { int major = 0, minor = 0, patch = 0; };

static SemVer parse_semver(std::string s) {
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.erase(0, 1);
    SemVer out;
    int* slot[3] = { &out.major, &out.minor, &out.patch };
    int idx = 0;
    std::string cur;
    auto flush = [&]() {
        if (idx > 2) return;
        try { *slot[idx] = std::stoi(cur); } catch (...) { *slot[idx] = 0; }
        ++idx;
        cur.clear();
    };
    for (char c : s) {
        if (c == '.') {
            flush();
        } else if (c >= '0' && c <= '9') {
            cur.push_back(c);
        } else {
            // Stop at any non-digit/non-dot (e.g. "0.3.4-beta1").
            break;
        }
    }
    if (!cur.empty()) flush();
    return out;
}

static bool is_newer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    return a.patch > b.patch;
}

// Naive JSON string-field extractor: finds `"key":"value"` and returns the
// value with basic backslash-escape handling. Sufficient for GitHub's
// release JSON where we only need tag_name and html_url.
static std::string extract_string_field(const std::string& json,
                                        const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
                               json[p] == '\r' || json[p] == '\n'))
        ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;
    std::string out;
    while (p < json.size()) {
        char c = json[p++];
        if (c == '\\' && p < json.size()) {
            char esc = json[p++];
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                default:   out.push_back(esc);  break;
            }
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return {};
}

#ifdef _WIN32
// HTTPS GET with WinHTTP. Returns body as std::string; out_status receives
// the HTTP status code (e.g. 200, 404, 0 on transport failure).
static std::string https_get(const wchar_t* host,
                             const wchar_t* path,
                             const wchar_t* user_agent,
                             DWORD* out_status,
                             std::string* out_error) {
    if (out_status) *out_status = 0;
    HINTERNET hSession = WinHttpOpen(user_agent,
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        if (out_error) *out_error = "WinHttpOpen failed";
        return {};
    }
    // Reasonable timeouts so a dead network can't hang the worker forever.
    // resolve / connect / send / receive (ms).
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 8000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        if (out_error) *out_error = "WinHttpConnect failed";
        WinHttpCloseHandle(hSession);
        return {};
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        if (out_error) *out_error = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // GitHub requires Accept header for the API to return JSON consistently.
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
    BOOL ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        if (out_error) *out_error = "WinHttpSendRequest/Receive failed";
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &status_size, WINHTTP_NO_HEADER_INDEX);
    if (out_status) *out_status = status;

    std::string body;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) break;
        body.append(buf.data(), read);
        if (body.size() > 2 * 1024 * 1024) break; // 2 MB safety cap
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (status >= 400) {
        if (out_error) {
            std::ostringstream oss;
            oss << "HTTP " << status;
            *out_error = oss.str();
        }
    }
    return body;
}
#endif

} // namespace

UpdateCheckResult check_for_update(const std::string& current_version) {
    UpdateCheckResult r;
    r.current_version = current_version;

#ifdef _WIN32
    DWORD status = 0;
    std::string err;
    std::string body = https_get(
        L"api.github.com",
        L"/repos/MSEndpointMgr/1PhoneMirror/releases/latest",
        L"1PhoneMirror-UpdateCheck/1.0",
        &status, &err);

    if (body.empty() || status != 200) {
        r.ok = false;
        r.error = err.empty() ? "no response" : err;
        return r;
    }

    std::string tag = extract_string_field(body, "tag_name");
    if (tag.empty()) {
        r.ok = false;
        r.error = "tag_name missing";
        return r;
    }
    std::string html = extract_string_field(body, "html_url");

    SemVer cur  = parse_semver(current_version);
    SemVer late = parse_semver(tag);

    r.ok               = true;
    r.latest_version   = tag.front() == 'v' ? tag.substr(1) : tag;
    r.release_url      = !html.empty()
                       ? html
                       : "https://github.com/MSEndpointMgr/1PhoneMirror/releases/latest";
    r.update_available = is_newer(late, cur);
    return r;
#else
    r.ok = false;
    r.error = "update check not implemented on this platform";
    return r;
#endif
}

} // namespace openmirror::network
