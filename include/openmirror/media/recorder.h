#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declarations from FFmpeg (avoid pulling its headers into this file).
extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;
}

namespace openmirror::media {

enum class RecordFormat {
    MP4 = 0,
    GIF = 1,
};

struct RecordConfig {
    RecordFormat format = RecordFormat::MP4;
    std::string  output_path;          // full path including filename
    int          width  = 0;           // source RGBA width
    int          height = 0;           // source RGBA height
    int          target_fps = 30;      // encoded fps (renderer drops to fit)
    int          max_duration_sec = 0; // 0 = unlimited; recorder auto-stops
    int          bitrate_kbps = 6000;  // MP4 only
    int          max_queue_frames = 8; // bounded queue (drop oldest if full)
};

// Encodes RGBA frames to MP4 (H.264) or GIF on a worker thread.
//
// Threading model:
//   * push_frame() is called from the renderer (main) thread for every
//     decoded video frame. It clones the RGBA buffer and pushes onto a
//     bounded queue. If the queue is full the oldest frame is dropped to
//     avoid stalling the UI.
//   * A worker thread pops frames, runs swscale (RGBA -> YUV420P or PAL8),
//     feeds the encoder, and writes packets to the muxer.
//   * stop() signals end-of-stream, waits for the worker to flush, and
//     finalises the file.
//
// The recorder is single-shot: call start() to begin a recording, stop()
// to finalise, then start() again with a new config for the next take.
class Recorder {
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder&)            = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Begin recording. Returns true on success, false on error (see
    // last_error()). Safe to call from any thread; blocks briefly while
    // initialising the encoder.
    bool start(const RecordConfig& cfg);

    // Finalise the current recording. Blocks until the worker drains and
    // the file is closed. Returns the output path on success, or an empty
    // string if recording failed (see last_error()).
    std::string stop();

    // Push a new frame from the renderer. width/height/stride describe the
    // source RGBA buffer. The recorder copies only what fits its target
    // dimensions; mismatched sizes are scaled by swscale on the worker.
    void push_frame(const uint8_t* rgba, int width, int height, int stride);

    bool is_recording() const { return recording_.load(); }

    // Wall-clock seconds since start() (0 if not recording).
    double elapsed_seconds() const;

    // Last human-readable error, or empty string on success.
    std::string last_error() const;

    // True when stop() should be called (max_duration reached, or fatal
    // error). Renderer polls this each frame.
    bool should_finalize() const { return should_finalize_.load(); }

    RecordFormat format() const { return cfg_.format; }

private:
    struct QueuedFrame {
        std::vector<uint8_t> rgba; // tightly packed (no stride padding)
        int width  = 0;
        int height = 0;
        std::chrono::steady_clock::time_point captured_at;
    };

    void worker_loop();
    bool init_encoder();           // called on the worker thread
    bool encode_frame(AVFrame* frame); // pushes into encoder, drains packets
    void teardown_encoder();
    void set_error(const std::string& msg);

    RecordConfig cfg_{};

    std::atomic<bool> recording_{false};
    std::atomic<bool> should_finalize_{false};

    // Frame queue
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueuedFrame> queue_;
    bool                    end_of_stream_ = false; // set under queue_mutex_

    // Worker
    std::thread worker_;
    std::chrono::steady_clock::time_point start_time_{};

    // Encoder state (owned by worker thread; touched only there once init
    // succeeds, except the destructor which joins the worker first).
    AVFormatContext* fmt_ctx_   = nullptr;
    AVCodecContext*  codec_ctx_ = nullptr;
    AVStream*        stream_    = nullptr;
    AVFrame*         enc_frame_ = nullptr;
    SwsContext*      sws_       = nullptr;
    int64_t          next_pts_  = 0;

    // Error state (touched from any thread).
    mutable std::mutex error_mutex_;
    std::string        error_msg_;
};

} // namespace openmirror::media
