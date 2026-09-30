#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;

namespace opm::media {

// Decoded frame ready for rendering
struct VideoFrame {
    uint8_t* data = nullptr;   // RGBA pixel data
    int width = 0;
    int height = 0;
    int stride = 0;
    int64_t pts = 0;

    ~VideoFrame();
    VideoFrame() = default;
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&& other) noexcept;
    VideoFrame& operator=(VideoFrame&& other) noexcept;
};

// Decoded audio samples
struct AudioFrame {
    std::unique_ptr<uint8_t[]> data;
    int size = 0;
    int sample_rate = 0;
    int channels = 0;
    int64_t pts = 0;
};

using OnVideoFrame = std::function<void(VideoFrame frame)>;
using OnAudioFrame = std::function<void(AudioFrame frame)>;

class Decoder {
public:
    Decoder();
    ~Decoder();

    // Initialize video decoder for H.264 or H.265
    bool init_video(int codec_id); // AV_CODEC_ID_H264 or AV_CODEC_ID_H265
    // extradata carries the raw AudioSpecificConfig for AAC-LC / AAC-ELD,
    // which FFmpeg needs up front because AirPlay sends bare AAC frames
    // with no in-band ADTS header.
    bool init_audio(int codec_id, int sample_rate, int channels,
                    const uint8_t* extradata = nullptr, int extradata_size = 0);

    // Feed encoded data, decoded frames are dispatched via callbacks
    bool decode_video(const uint8_t* data, size_t size, int64_t pts);
    bool decode_audio(const uint8_t* data, size_t size, int64_t pts);

    void set_video_callback(OnVideoFrame cb) { on_video_frame_ = std::move(cb); }
    void set_audio_callback(OnAudioFrame cb) { on_audio_frame_ = std::move(cb); }

    void flush();

private:
    bool decode_video_frame(AVPacket* pkt);
    bool decode_audio_frame(AVPacket* pkt);

    AVCodecContext* video_ctx_ = nullptr;
    AVCodecContext* audio_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    int swr_src_fmt_ = -1;
    int swr_src_rate_ = 0;
    int swr_src_channels_ = 0;

    OnVideoFrame on_video_frame_;
    OnAudioFrame on_audio_frame_;

    int output_width_ = 0;
    int output_height_ = 0;

    std::mutex video_mutex_;
    std::mutex audio_mutex_;
};

} // namespace opm::media
