#pragma once

// Webcam capture for the "hands & hardware" drawer (see TODO.md #9).
//
// Windows-only. Built when ENABLE_WEBCAM is defined (CMake option).
// Uses Media Foundation directly to keep dependencies thin (mf, mfplat,
// mfreadwrite, mfuuid, shlwapi — all in the platform SDK).
//
// Milestone 2: enumerate() + live capture pump via IMFSourceReader.
// Worker thread owns all COM resources; main thread polls get_latest()
// from the SDL render loop.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opm::media {

struct WebcamDeviceInfo {
    std::string id;    // MF symbolic link (stable across reboots)
    std::string name;  // friendly name shown in the picker
};

struct WebcamFrame {
    std::vector<uint8_t> rgba;   // tightly packed, width*height*4
    int width  = 0;
    int height = 0;
    std::chrono::steady_clock::time_point captured_at;
};

class WebcamCapture {
public:
    WebcamCapture();
    ~WebcamCapture();

    WebcamCapture(const WebcamCapture&)            = delete;
    WebcamCapture& operator=(const WebcamCapture&) = delete;

    // Enumerate webcams currently visible to Media Foundation. Safe to
    // call any number of times, on any thread, before or after start().
    // Returns an empty list when ENABLE_WEBCAM is off, MF is unavailable,
    // or the user has revoked camera access globally.
    static std::vector<WebcamDeviceInfo> enumerate();

    // Begin capturing from `device_id` (MF symbolic link from enumerate()).
    // Empty `device_id` picks the system default. preferred_width/height
    // are hints; the driver picks the closest supported mode. Returns
    // false on error — call last_error() for details. Blocks briefly
    // (a few hundred ms) while MF activates the device and negotiates
    // the output format.
    bool start(const std::string& device_id,
               int preferred_width  = 1280,
               int preferred_height = 720);

    // Stop capture and join the worker thread. Safe to call when not
    // running.
    void stop();

    bool is_running() const { return running_.load(); }

    // Non-blocking: copies the latest captured frame into `out` if one is
    // available since the last get_latest() call. Returns false when no
    // new frame is ready. Renderer polls this each tick from the SDL
    // thread, so it must be fast.
    bool get_latest(WebcamFrame& out);

    // Last human-readable error, or empty string on success.
    std::string last_error() const;

    // Resolution the driver negotiated. Zero until the first frame
    // arrives.
    int width()  const { return current_w_.load(); }
    int height() const { return current_h_.load(); }

private:
    void set_error(const std::string& msg);

    // Capture worker entry point. Runs on `worker_`. Signals `ready`
    // once init has succeeded (running_ = true) or failed. After that,
    // pumps IMFSourceReader::ReadSample until running_ flips to false.
    void run_capture_worker(std::string device_id,
                            int preferred_width,
                            int preferred_height,
                            std::promise<bool> ready);

    std::atomic<bool> running_{false};
    std::atomic<int>  current_w_{0};
    std::atomic<int>  current_h_{0};

    std::thread       worker_;
    std::mutex        latest_mutex_;
    WebcamFrame       latest_;
    bool              latest_dirty_ = false;

    mutable std::mutex err_mutex_;
    std::string        last_error_;
};

} // namespace opm::media
