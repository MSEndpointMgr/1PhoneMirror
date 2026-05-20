// Webcam capture — Milestone 1: device enumeration only.
//
// Capture pump (start/stop/get_latest) lands in milestone 2 once the
// drawer UI is in place. Keeping the file Windows-only via ENABLE_WEBCAM.

#include <opm/media/webcam.h>

#ifdef ENABLE_WEBCAM

#include <iostream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace opm::media {

namespace {

// RAII guard for MFStartup/MFShutdown. enumerate() may be called before
// any other MF work, so we ensure the platform is initialised for the
// duration of the call. Reference-counted internally by MF, so nesting
// is fine if Renderer also calls MFStartup later for the capture pump.
struct MfPlatformGuard {
    HRESULT hr;
    MfPlatformGuard() {
        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    }
    ~MfPlatformGuard() {
        if (SUCCEEDED(hr)) MFShutdown();
    }
};

// Convert a UTF-16 LPWSTR (from MF) to UTF-8 std::string for our logs
// and settings file. Returns empty string on failure or null input.
std::string wide_to_utf8(LPCWSTR w) {
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

std::vector<WebcamDeviceInfo> WebcamCapture::enumerate() {
    std::vector<WebcamDeviceInfo> result;

    MfPlatformGuard mf;
    if (FAILED(mf.hr)) {
        std::cerr << "[Webcam] MFStartup failed: 0x" << std::hex << mf.hr
                  << std::dec << "\n";
        return result;
    }

    IMFAttributes* attrs = nullptr;
    HRESULT hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr) || !attrs) {
        std::cerr << "[Webcam] MFCreateAttributes failed: 0x" << std::hex
                  << hr << std::dec << "\n";
        return result;
    }
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attrs, &activates, &count);
    attrs->Release();
    if (FAILED(hr)) {
        std::cerr << "[Webcam] MFEnumDeviceSources failed: 0x" << std::hex
                  << hr << std::dec << "\n";
        return result;
    }

    result.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        IMFActivate* act = activates[i];
        if (!act) continue;

        WebcamDeviceInfo info;

        // Friendly name (e.g. "Logi C920 HD Pro Webcam"). May be missing
        // for some virtual drivers — fall back to a generic label.
        WCHAR* name = nullptr;
        UINT32 name_len = 0;
        if (SUCCEEDED(act->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &name_len))) {
            info.name = wide_to_utf8(name);
            CoTaskMemFree(name);
        }
        if (info.name.empty()) {
            info.name = "Camera " + std::to_string(i);
        }

        // Symbolic link — the stable identifier we persist in settings.
        WCHAR* link = nullptr;
        UINT32 link_len = 0;
        if (SUCCEEDED(act->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                &link, &link_len))) {
            info.id = wide_to_utf8(link);
            CoTaskMemFree(link);
        }

        if (!info.id.empty()) {
            result.push_back(std::move(info));
        }

        act->Release();
    }
    CoTaskMemFree(activates);

    return result;
}

WebcamCapture::WebcamCapture()  = default;
WebcamCapture::~WebcamCapture() { stop(); }

bool WebcamCapture::start(const std::string& /*device_id*/,
                          int /*preferred_width*/,
                          int /*preferred_height*/) {
    set_error("Webcam capture pump not implemented yet (milestone 2).");
    return false;
}

void WebcamCapture::stop() {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(latest_mutex_);
        latest_         = {};
        latest_dirty_   = false;
    }
    current_w_.store(0);
    current_h_.store(0);
}

bool WebcamCapture::get_latest(WebcamFrame& out) {
    std::lock_guard<std::mutex> lock(latest_mutex_);
    if (!latest_dirty_) return false;
    out           = latest_;
    latest_dirty_ = false;
    return true;
}

std::string WebcamCapture::last_error() const {
    std::lock_guard<std::mutex> lock(err_mutex_);
    return last_error_;
}

void WebcamCapture::set_error(const std::string& msg) {
    std::lock_guard<std::mutex> lock(err_mutex_);
    last_error_ = msg;
    if (!msg.empty()) {
        std::cerr << "[Webcam] " << msg << "\n";
    }
}

} // namespace opm::media

#else  // !ENABLE_WEBCAM

// Stub implementations so non-Windows / no-webcam builds still link.
namespace opm::media {

std::vector<WebcamDeviceInfo> WebcamCapture::enumerate() { return {}; }

WebcamCapture::WebcamCapture()  = default;
WebcamCapture::~WebcamCapture() = default;

bool WebcamCapture::start(const std::string&, int, int) {
    set_error("Webcam support not compiled in (ENABLE_WEBCAM=OFF).");
    return false;
}
void WebcamCapture::stop() {}
bool WebcamCapture::get_latest(WebcamFrame&) { return false; }
std::string WebcamCapture::last_error() const {
    std::lock_guard<std::mutex> lock(err_mutex_);
    return last_error_;
}
void WebcamCapture::set_error(const std::string& msg) {
    std::lock_guard<std::mutex> lock(err_mutex_);
    last_error_ = msg;
}

} // namespace opm::media

#endif // ENABLE_WEBCAM
