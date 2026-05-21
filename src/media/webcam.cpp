// Webcam capture — Milestone 2: device enumeration + IMFSourceReader
// capture pump. Worker thread owns all COM/MF resources.
//
// Keeping the file Windows-only via ENABLE_WEBCAM. The non-Windows
// branch below provides empty stubs so the rest of the app links.

#include <opm/media/webcam.h>

#ifdef ENABLE_WEBCAM

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <objbase.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace opm::media {

namespace {

// RAII guard for MFStartup/MFShutdown. enumerate() may be called before
// any other MF work, so we ensure the platform is initialised for the
// duration of the call. Reference-counted internally by MF, so the
// capture worker can also call MFStartup safely.
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

// Convert UTF-8 to wide for MF symbolic-link comparison.
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
    return out;
}

// Locate an IMFActivate matching `device_id` (MF symbolic link, UTF-8).
// Empty `device_id` returns the first enumerated camera. Caller owns
// the returned activate (Release()) and all other activates in the
// returned array are released here. Returns nullptr on failure.
IMFActivate* find_device_activate(const std::string& device_id) {
    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return nullptr;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs, &acts, &count);
    attrs->Release();
    if (FAILED(hr) || count == 0) {
        if (acts) CoTaskMemFree(acts);
        return nullptr;
    }

    IMFActivate* pick = nullptr;
    for (UINT32 i = 0; i < count; ++i) {
        IMFActivate* a = acts[i];
        if (!a) continue;
        if (!pick) {
            if (device_id.empty()) {
                pick = a;
                pick->AddRef();
            } else {
                WCHAR* link = nullptr;
                UINT32 ll   = 0;
                if (SUCCEEDED(a->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        &link, &ll))) {
                    if (wide_to_utf8(link) == device_id) {
                        pick = a;
                        pick->AddRef();
                    }
                    CoTaskMemFree(link);
                }
            }
        }
        a->Release();
    }
    CoTaskMemFree(acts);
    return pick;
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

bool WebcamCapture::start(const std::string& device_id,
                          int preferred_width,
                          int preferred_height) {
    if (running_.load()) {
        set_error("Webcam capture is already running.");
        return false;
    }
    // Clear any previous error so the caller sees a fresh slate.
    set_error("");

    std::promise<bool> ready;
    auto ready_fut = ready.get_future();

    worker_ = std::thread(
        [this, device_id, preferred_width, preferred_height,
         ready = std::move(ready)]() mutable {
            run_capture_worker(device_id, preferred_width,
                               preferred_height, std::move(ready));
        });

    const bool ok = ready_fut.get();
    if (!ok && worker_.joinable()) {
        worker_.join();
    }
    return ok;
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

// ---------------------------------------------------------------------------
// Worker thread: owns all COM/MF resources for one capture session.
// Signals `ready` once init has either succeeded (running_=true) or failed
// (with last_error_ set). After that, drives ReadSample until stop() flips
// running_ to false.
// ---------------------------------------------------------------------------
void WebcamCapture::run_capture_worker(std::string device_id,
                                       int preferred_w,
                                       int preferred_h,
                                       std::promise<bool> ready) {
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_owned = SUCCEEDED(co_hr);

    auto fail = [&](const std::string& msg) {
        set_error(msg);
        ready.set_value(false);
        if (co_owned) CoUninitialize();
    };

    MfPlatformGuard mf;
    if (FAILED(mf.hr)) {
        fail("MFStartup failed in capture worker.");
        return;
    }

    IMFActivate* activate = find_device_activate(device_id);
    if (!activate) {
        fail(device_id.empty()
                 ? std::string("No webcam available.")
                 : std::string("Webcam not found: ") + device_id);
        return;
    }

    IMFMediaSource* source = nullptr;
    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&source));
    activate->Release();
    if (FAILED(hr) || !source) {
        fail("Failed to activate webcam (already in use?). HRESULT="
             + std::to_string(hr));
        return;
    }

    // Enable the built-in video processor so unusual native formats
    // (MJPG, NV12, etc.) get auto-converted to our RGB32 output.
    IMFAttributes* reader_attrs = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&reader_attrs, 1))) {
        reader_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }

    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromMediaSource(source, reader_attrs, &reader);
    if (reader_attrs) reader_attrs->Release();
    if (FAILED(hr) || !reader) {
        source->Shutdown();
        source->Release();
        fail("MFCreateSourceReaderFromMediaSource failed.");
        return;
    }

    // Pick the native frame size closest to the caller's preference.
    UINT32 best_w = 0, best_h = 0;
    int best_score = INT_MAX;
    for (DWORD i = 0; ; ++i) {
        IMFMediaType* nt = nullptr;
        hr = reader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &nt);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr) || !nt) break;
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(nt, MF_MT_FRAME_SIZE, &w, &h))
            && w > 0 && h > 0) {
            const int score = std::abs((int)w - preferred_w)
                            + std::abs((int)h - preferred_h);
            if (score < best_score) {
                best_score = score;
                best_w     = w;
                best_h     = h;
            }
        }
        nt->Release();
    }
    if (best_w == 0 || best_h == 0) {
        reader->Release();
        source->Shutdown();
        source->Release();
        fail("Webcam exposed no usable video formats.");
        return;
    }

    // Configure RGB32 output at the chosen size. The source reader will
    // chain a converter MFT automatically if needed.
    IMFMediaType* out_type = nullptr;
    hr = MFCreateMediaType(&out_type);
    if (FAILED(hr)) {
        reader->Release();
        source->Shutdown();
        source->Release();
        fail("MFCreateMediaType failed.");
        return;
    }
    out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(out_type, MF_MT_FRAME_SIZE, best_w, best_h);
    out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    hr = reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out_type);
    out_type->Release();
    if (FAILED(hr)) {
        reader->Release();
        source->Shutdown();
        source->Release();
        fail("SetCurrentMediaType(RGB32) failed. HRESULT="
             + std::to_string(hr));
        return;
    }

    // Determine stride from the negotiated current type. RGB32 surfaces
    // are usually bottom-up (negative stride) when GDI is in the path.
    LONG row_pitch  = (LONG)best_w * 4;
    bool bottom_up  = false;
    {
        IMFMediaType* cur = nullptr;
        if (SUCCEEDED(reader->GetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) && cur) {
            UINT32 stride_raw = 0;
            if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_raw))) {
                LONG s = (LONG)(INT32)stride_raw;
                if (s < 0) { bottom_up = true; s = -s; }
                if (s > 0) row_pitch = s;
            }
            cur->Release();
        }
    }

    current_w_.store((int)best_w);
    current_h_.store((int)best_h);
    running_.store(true);
    ready.set_value(true);

    std::cout << "[Webcam] Capture started: " << best_w << "x" << best_h
              << (bottom_up ? " (bottom-up)" : "") << "\n";

    // ----- read loop -------------------------------------------------------
    while (running_.load()) {
        DWORD       stream_idx = 0;
        DWORD       flags      = 0;
        LONGLONG    ts         = 0;
        IMFSample*  sample     = nullptr;

        hr = reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
            &stream_idx, &flags, &ts, &sample);
        if (FAILED(hr)) {
            set_error("ReadSample failed; stopping capture. HRESULT="
                      + std::to_string(hr));
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            set_error("Webcam stream ended.");
            if (sample) sample->Release();
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            // Re-query negotiated size; format itself stays RGB32.
            IMFMediaType* cur = nullptr;
            if (SUCCEEDED(reader->GetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) && cur) {
                UINT32 w = 0, h = 0;
                if (SUCCEEDED(MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h))
                    && w > 0 && h > 0) {
                    best_w    = w;
                    best_h    = h;
                    row_pitch = (LONG)w * 4;
                    UINT32 sr = 0;
                    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &sr))) {
                        LONG s = (LONG)(INT32)sr;
                        bottom_up = (s < 0);
                        if (s < 0) s = -s;
                        if (s > 0) row_pitch = s;
                    }
                    current_w_.store((int)w);
                    current_h_.store((int)h);
                }
                cur->Release();
            }
        }
        if (!sample) continue;

        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
            BYTE* data = nullptr;
            DWORD cur_len = 0;
            if (SUCCEEDED(buf->Lock(&data, nullptr, &cur_len)) && data) {
                const int W = (int)best_w;
                const int H = (int)best_h;
                WebcamFrame frame;
                frame.width  = W;
                frame.height = H;
                frame.rgba.resize(size_t(W) * size_t(H) * 4);

                for (int y = 0; y < H; ++y) {
                    const int src_y = bottom_up ? (H - 1 - y) : y;
                    const uint8_t* sr = data + (size_t)src_y * (size_t)row_pitch;
                    uint8_t*       dr = frame.rgba.data() + (size_t)y * (size_t)W * 4;
                    // MFVideoFormat_RGB32 byte order: B, G, R, X.
                    for (int x = 0; x < W; ++x) {
                        dr[x * 4 + 0] = sr[x * 4 + 2]; // R
                        dr[x * 4 + 1] = sr[x * 4 + 1]; // G
                        dr[x * 4 + 2] = sr[x * 4 + 0]; // B
                        dr[x * 4 + 3] = 0xFF;
                    }
                }
                frame.captured_at = std::chrono::steady_clock::now();

                buf->Unlock();

                std::lock_guard<std::mutex> lk(latest_mutex_);
                latest_       = std::move(frame);
                latest_dirty_ = true;
            }
            buf->Release();
        }
        sample->Release();
    }

    // ----- shutdown --------------------------------------------------------
    running_.store(false);
    reader->Release();
    source->Shutdown();
    source->Release();

    std::cout << "[Webcam] Capture stopped.\n";
    if (co_owned) CoUninitialize();
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

// run_capture_worker is Windows-only; provide an empty definition so
// the symbol exists even when the header declares it unconditionally.
void WebcamCapture::run_capture_worker(std::string, int, int,
                                       std::promise<bool> ready) {
    ready.set_value(false);
}

} // namespace opm::media

#endif // ENABLE_WEBCAM
