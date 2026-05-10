#include <openmirror/media/recorder.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace openmirror::media {

namespace {

// Look up a video encoder by preference. For MP4 we want H.264; we try the
// software libx264 (best quality for the bitrate), then h264_mf (Windows
// Media Foundation, hardware-accelerated when available), and finally fall
// back to mpeg4 which is always present in any FFmpeg build.
const AVCodec* find_video_encoder(RecordFormat fmt) {
    if (fmt == RecordFormat::GIF) {
        return avcodec_find_encoder(AV_CODEC_ID_GIF);
    }
    // MP4 path
    if (auto c = avcodec_find_encoder_by_name("libx264")) return c;
    if (auto c = avcodec_find_encoder_by_name("h264_mf")) return c;
    if (auto c = avcodec_find_encoder(AV_CODEC_ID_MPEG4)) return c;
    return nullptr;
}

const char* container_for(RecordFormat fmt) {
    return fmt == RecordFormat::GIF ? "gif" : "mp4";
}

// FFmpeg uses negative AVERROR codes; render a friendly message.
std::string av_err(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

} // namespace

Recorder::Recorder() = default;

Recorder::~Recorder() {
    if (recording_.load() || worker_.joinable()) {
        // Defensive cleanup if caller forgot to stop().
        stop();
    }
}

bool Recorder::start(const RecordConfig& cfg) {
    if (recording_.load()) {
        set_error("recorder already running");
        return false;
    }
    if (cfg.width <= 0 || cfg.height <= 0 || cfg.output_path.empty()) {
        set_error("invalid recorder config (width/height/output_path required)");
        return false;
    }

    cfg_ = cfg;
    // Force even dimensions for H.264 (yuv420p sub-sampling needs them).
    if (cfg_.format == RecordFormat::MP4) {
        cfg_.width  &= ~1;
        cfg_.height &= ~1;
    }
    if (cfg_.target_fps <= 0) cfg_.target_fps = 30;
    if (cfg_.max_queue_frames <= 0) cfg_.max_queue_frames = 8;

    {
        std::lock_guard<std::mutex> lk(error_mutex_);
        error_msg_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        queue_.clear();
        end_of_stream_ = false;
    }
    should_finalize_.store(false);
    next_pts_ = 0;
    start_time_ = std::chrono::steady_clock::now();

    recording_.store(true);
    worker_ = std::thread(&Recorder::worker_loop, this);
    return true;
}

std::string Recorder::stop() {
    if (!recording_.load() && !worker_.joinable()) {
        return {};
    }

    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        end_of_stream_ = true;
    }
    queue_cv_.notify_all();

    if (worker_.joinable()) worker_.join();
    recording_.store(false);
    should_finalize_.store(false);

    if (!last_error().empty()) return {};
    return cfg_.output_path;
}

void Recorder::push_frame(const uint8_t* rgba, int width, int height, int stride) {
    if (!recording_.load() || rgba == nullptr || width <= 0 || height <= 0) return;
    if (should_finalize_.load()) return;

    QueuedFrame qf;
    qf.width  = width;
    qf.height = height;
    qf.captured_at = std::chrono::steady_clock::now();
    qf.rgba.resize(static_cast<size_t>(width) * height * 4);
    if (stride == width * 4) {
        std::memcpy(qf.rgba.data(), rgba, qf.rgba.size());
    } else {
        for (int y = 0; y < height; ++y) {
            std::memcpy(qf.rgba.data() + static_cast<size_t>(y) * width * 4,
                        rgba + static_cast<size_t>(y) * stride,
                        static_cast<size_t>(width) * 4);
        }
    }

    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        // Drop oldest if the worker is falling behind. Encoding is the
        // bottleneck under load — better to lose a frame than to stall the
        // renderer.
        while ((int)queue_.size() >= cfg_.max_queue_frames) {
            queue_.pop_front();
        }
        queue_.push_back(std::move(qf));
    }
    queue_cv_.notify_one();
}

double Recorder::elapsed_seconds() const {
    if (!recording_.load()) return 0.0;
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_time_).count();
}

std::string Recorder::last_error() const {
    std::lock_guard<std::mutex> lk(error_mutex_);
    return error_msg_;
}

void Recorder::set_error(const std::string& msg) {
    std::lock_guard<std::mutex> lk(error_mutex_);
    if (error_msg_.empty()) error_msg_ = msg;
    std::cerr << "[Recorder] " << msg << "\n";
}

bool Recorder::init_encoder() {
    int ret = 0;

    // ---- Output format ----
    ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr,
                                         container_for(cfg_.format),
                                         cfg_.output_path.c_str());
    if (ret < 0 || !fmt_ctx_) {
        set_error("avformat_alloc_output_context2 failed: " + av_err(ret));
        return false;
    }

    // ---- Encoder ----
    const AVCodec* codec = find_video_encoder(cfg_.format);
    if (!codec) {
        set_error("no suitable video encoder found in this FFmpeg build");
        return false;
    }
    std::cout << "[Recorder] Using encoder: " << codec->name << "\n";

    stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if (!stream_) {
        set_error("avformat_new_stream failed");
        return false;
    }
    stream_->id = (int)(fmt_ctx_->nb_streams - 1);

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        set_error("avcodec_alloc_context3 failed");
        return false;
    }

    codec_ctx_->width      = cfg_.width;
    codec_ctx_->height     = cfg_.height;
    codec_ctx_->time_base  = AVRational{1, cfg_.target_fps};
    codec_ctx_->framerate  = AVRational{cfg_.target_fps, 1};
    codec_ctx_->gop_size   = cfg_.target_fps * 2;       // keyframe every 2 s
    codec_ctx_->max_b_frames = 0;

    AVPixelFormat target_pix_fmt;
    if (cfg_.format == RecordFormat::MP4) {
        target_pix_fmt = AV_PIX_FMT_YUV420P;
        codec_ctx_->pix_fmt   = target_pix_fmt;
        codec_ctx_->bit_rate  = (int64_t)cfg_.bitrate_kbps * 1000;
        // libx264-specific tuning
        if (std::string(codec->name) == "libx264") {
            av_opt_set(codec_ctx_->priv_data, "preset", "veryfast", 0);
            av_opt_set(codec_ctx_->priv_data, "tune",   "zerolatency", 0);
            av_opt_set(codec_ctx_->priv_data, "profile", "high", 0);
        }
    } else {
        // GIF: encode as 8-bit RGB332 (AV_PIX_FMT_RGB8). The built-in GIF
        // encoder accepts this directly and swscale can convert RGBA->RGB8
        // with ordered dithering, no palettegen filter required. Quality
        // is "fine for screen-recording demos" rather than "publishable
        // GIF art" — for higher fidelity we could swap in palettegen +
        // paletteuse later via libavfilter.
        target_pix_fmt = AV_PIX_FMT_RGB8;
        codec_ctx_->pix_fmt = target_pix_fmt;
    }

    if (fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        set_error(std::string("avcodec_open2 failed (") + codec->name + "): " + av_err(ret));
        return false;
    }

    ret = avcodec_parameters_from_context(stream_->codecpar, codec_ctx_);
    if (ret < 0) {
        set_error("avcodec_parameters_from_context failed: " + av_err(ret));
        return false;
    }
    stream_->time_base = codec_ctx_->time_base;

    // ---- Open output file ----
    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmt_ctx_->pb, cfg_.output_path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            set_error("avio_open failed for '" + cfg_.output_path + "': " + av_err(ret));
            return false;
        }
    }

    ret = avformat_write_header(fmt_ctx_, nullptr);
    if (ret < 0) {
        set_error("avformat_write_header failed: " + av_err(ret));
        return false;
    }

    // ---- Reusable encoder frame ----
    enc_frame_ = av_frame_alloc();
    if (!enc_frame_) {
        set_error("av_frame_alloc failed");
        return false;
    }
    enc_frame_->format = target_pix_fmt;
    enc_frame_->width  = cfg_.width;
    enc_frame_->height = cfg_.height;
    ret = av_frame_get_buffer(enc_frame_, 32);
    if (ret < 0) {
        set_error("av_frame_get_buffer failed: " + av_err(ret));
        return false;
    }

    // sws_ is created lazily on the first frame because the source size
    // may differ between captures (resize).
    return true;
}

bool Recorder::encode_frame(AVFrame* frame) {
    int ret = avcodec_send_frame(codec_ctx_, frame);
    if (ret < 0 && ret != AVERROR_EOF) {
        set_error("avcodec_send_frame failed: " + av_err(ret));
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    while (true) {
        ret = avcodec_receive_packet(codec_ctx_, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            set_error("avcodec_receive_packet failed: " + av_err(ret));
            av_packet_free(&pkt);
            return false;
        }
        av_packet_rescale_ts(pkt, codec_ctx_->time_base, stream_->time_base);
        pkt->stream_index = stream_->index;
        ret = av_interleaved_write_frame(fmt_ctx_, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            set_error("av_interleaved_write_frame failed: " + av_err(ret));
            av_packet_free(&pkt);
            return false;
        }
    }
    av_packet_free(&pkt);
    return true;
}

void Recorder::teardown_encoder() {
    if (codec_ctx_) {
        // Drain encoder
        encode_frame(nullptr);
    }
    if (fmt_ctx_) {
        if (fmt_ctx_->pb) av_write_trailer(fmt_ctx_);
        if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE) && fmt_ctx_->pb) {
            avio_closep(&fmt_ctx_->pb);
        }
    }
    if (sws_)        { sws_freeContext(sws_); sws_ = nullptr; }
    if (enc_frame_)  { av_frame_free(&enc_frame_); }
    if (codec_ctx_)  { avcodec_free_context(&codec_ctx_); }
    if (fmt_ctx_)    { avformat_free_context(fmt_ctx_); fmt_ctx_ = nullptr; }
    stream_ = nullptr;
}

void Recorder::worker_loop() {
    if (!init_encoder()) {
        // Encoder init failed — drain queue and bail. Set should_finalize_
        // so the renderer notices and triggers UI cleanup.
        teardown_encoder();
        should_finalize_.store(true);
        return;
    }

    // Frame-rate gating: only encode every Nth incoming frame so that we
    // hit the configured target fps regardless of source framerate.
    const auto frame_interval = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<double>(1.0 / cfg_.target_fps));
    auto next_capture_time = std::chrono::steady_clock::now();

    AVPixelFormat target_pix_fmt = (cfg_.format == RecordFormat::GIF)
        ? AV_PIX_FMT_RGB8 : AV_PIX_FMT_YUV420P;

    while (true) {
        QueuedFrame qf;
        {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            queue_cv_.wait(lk, [&] { return !queue_.empty() || end_of_stream_; });
            if (queue_.empty() && end_of_stream_) break;
            qf = std::move(queue_.front());
            queue_.pop_front();
        }

        // Frame-rate decimation: skip if we already encoded a frame for the
        // current 1/fps slot.
        auto now = std::chrono::steady_clock::now();
        if (now < next_capture_time) {
            // Too early — drop this source frame.
            continue;
        }
        next_capture_time = now + frame_interval;

        // Lazy swscale init / re-init when source dims change.
        if (!sws_ || enc_frame_->width != cfg_.width || enc_frame_->height != cfg_.height) {
            if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
            sws_ = sws_getContext(qf.width, qf.height, AV_PIX_FMT_RGBA,
                                  cfg_.width, cfg_.height, target_pix_fmt,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws_) {
                set_error("sws_getContext failed");
                break;
            }
        }

        // Make encoder frame writable (av_frame_make_writable).
        if (av_frame_make_writable(enc_frame_) < 0) {
            set_error("av_frame_make_writable failed");
            break;
        }

        const uint8_t* src_data[1]    = { qf.rgba.data() };
        int            src_linesize[1] = { qf.width * 4 };
        sws_scale(sws_, src_data, src_linesize, 0, qf.height,
                  enc_frame_->data, enc_frame_->linesize);

        enc_frame_->pts = next_pts_++;

        if (!encode_frame(enc_frame_)) break;

        // Auto-stop on max duration.
        if (cfg_.max_duration_sec > 0 &&
            elapsed_seconds() >= (double)cfg_.max_duration_sec) {
            should_finalize_.store(true);
            break;
        }
    }

    teardown_encoder();
}

} // namespace openmirror::media
