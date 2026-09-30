#include <opm/media/decoder.h>
#include <cstring>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace opm::media {

// --- VideoFrame ---

VideoFrame::~VideoFrame() {
    if (data) {
        av_free(data);
        data = nullptr;
    }
}

VideoFrame::VideoFrame(VideoFrame&& other) noexcept
    : data(other.data), width(other.width), height(other.height),
      stride(other.stride), pts(other.pts) {
    other.data = nullptr;
}

VideoFrame& VideoFrame::operator=(VideoFrame&& other) noexcept {
    if (this != &other) {
        if (data) av_free(data);
        data = other.data;
        width = other.width;
        height = other.height;
        stride = other.stride;
        pts = other.pts;
        other.data = nullptr;
    }
    return *this;
}

// --- Decoder ---

Decoder::Decoder() = default;

Decoder::~Decoder() {
    flush();
    if (video_ctx_) avcodec_free_context(&video_ctx_);
    if (audio_ctx_) avcodec_free_context(&audio_ctx_);
    if (sws_ctx_) sws_freeContext(sws_ctx_);
    if (swr_ctx_) swr_free(&swr_ctx_);
}

bool Decoder::init_video(int codec_id) {
    const AVCodec* codec = avcodec_find_decoder(static_cast<AVCodecID>(codec_id));
    if (!codec) {
        std::cerr << "[Decoder] Video codec not found: " << codec_id << "\n";
        return false;
    }

    video_ctx_ = avcodec_alloc_context3(codec);
    if (!video_ctx_) {
        std::cerr << "[Decoder] Failed to allocate video codec context\n";
        return false;
    }

    // Allow for multithreaded decoding
    video_ctx_->thread_count = 4;
    video_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    if (avcodec_open2(video_ctx_, codec, nullptr) < 0) {
        std::cerr << "[Decoder] Failed to open video codec\n";
        avcodec_free_context(&video_ctx_);
        return false;
    }

    std::cout << "[Decoder] Video decoder initialized: " << codec->name << "\n";
    return true;
}

bool Decoder::init_audio(int codec_id, int sample_rate, int channels,
                         const uint8_t* extradata, int extradata_size) {
    const AVCodec* codec = avcodec_find_decoder(static_cast<AVCodecID>(codec_id));
    if (!codec) {
        std::cerr << "[Decoder] Audio codec not found: " << codec_id << "\n";
        return false;
    }

    audio_ctx_ = avcodec_alloc_context3(codec);
    if (!audio_ctx_) {
        std::cerr << "[Decoder] Failed to allocate audio codec context\n";
        return false;
    }

    audio_ctx_->sample_rate = sample_rate;
    av_channel_layout_default(&audio_ctx_->ch_layout, channels);
    audio_ctx_->thread_count = 2;

    if (extradata && extradata_size > 0) {
        audio_ctx_->extradata = static_cast<uint8_t*>(
            av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!audio_ctx_->extradata) {
            avcodec_free_context(&audio_ctx_);
            return false;
        }
        std::memcpy(audio_ctx_->extradata, extradata, extradata_size);
        audio_ctx_->extradata_size = extradata_size;
    }

    if (avcodec_open2(audio_ctx_, codec, nullptr) < 0) {
        std::cerr << "[Decoder] Failed to open audio codec\n";
        avcodec_free_context(&audio_ctx_);
        return false;
    }

    std::cout << "[Decoder] Audio decoder initialized: " << codec->name << "\n";
    return true;
}

bool Decoder::decode_video(const uint8_t* data, size_t size, int64_t pts) {
    std::lock_guard lock(video_mutex_);
    if (!video_ctx_) return false;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    // Wrap the input buffer without copying
    pkt->data = const_cast<uint8_t*>(data);
    pkt->size = static_cast<int>(size);
    pkt->pts = pts;
    pkt->dts = pts;

    bool result = decode_video_frame(pkt);
    av_packet_free(&pkt);
    return result;
}

bool Decoder::decode_audio(const uint8_t* data, size_t size, int64_t pts) {
    std::lock_guard lock(audio_mutex_);
    if (!audio_ctx_) return false;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    pkt->data = const_cast<uint8_t*>(data);
    pkt->size = static_cast<int>(size);
    pkt->pts = pts;

    bool result = decode_audio_frame(pkt);
    av_packet_free(&pkt);
    return result;
}

bool Decoder::decode_video_frame(AVPacket* pkt) {
    int ret = avcodec_send_packet(video_ctx_, pkt);
    if (ret < 0) {
        std::cerr << "[Decoder] Error sending video packet: " << ret << "\n";
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    while (avcodec_receive_frame(video_ctx_, frame) == 0) {
        // Lazily init/update the scaler when resolution changes
        if (frame->width != output_width_ || frame->height != output_height_) {
            if (sws_ctx_) sws_freeContext(sws_ctx_);
            sws_ctx_ = sws_getContext(
                frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                frame->width, frame->height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            output_width_ = frame->width;
            output_height_ = frame->height;
        }

        // Convert to RGBA
        VideoFrame vf;
        vf.width = frame->width;
        vf.height = frame->height;
        vf.stride = frame->width * 4;
        vf.pts = frame->pts;
        int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, vf.width, vf.height, 1);
        vf.data = static_cast<uint8_t*>(av_malloc(buf_size));

        uint8_t* dst_data[1] = { vf.data };
        int dst_linesize[1] = { vf.stride };
        sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height,
                  dst_data, dst_linesize);

        if (on_video_frame_) {
            on_video_frame_(std::move(vf));
        }

        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    return true;
}

bool Decoder::decode_audio_frame(AVPacket* pkt) {
    int ret = avcodec_send_packet(audio_ctx_, pkt);
    if (ret < 0) return false;

    AVFrame* frame = av_frame_alloc();
    while (avcodec_receive_frame(audio_ctx_, frame) == 0) {
        const int channels = frame->ch_layout.nb_channels;

        // (Re)build the resampler whenever the decoder's output format changes.
        if (!swr_ctx_ || swr_src_fmt_ != frame->format ||
            swr_src_rate_ != frame->sample_rate || swr_src_channels_ != channels) {
            if (swr_ctx_) swr_free(&swr_ctx_);
            AVChannelLayout out_layout;
            av_channel_layout_default(&out_layout, channels);
            if (swr_alloc_set_opts2(&swr_ctx_,
                                    &out_layout, AV_SAMPLE_FMT_S16, frame->sample_rate,
                                    &frame->ch_layout,
                                    static_cast<AVSampleFormat>(frame->format),
                                    frame->sample_rate,
                                    0, nullptr) < 0 ||
                swr_init(swr_ctx_) < 0) {
                std::cerr << "[Decoder] Failed to init audio resampler\n";
                if (swr_ctx_) swr_free(&swr_ctx_);
                av_channel_layout_uninit(&out_layout);
                av_frame_unref(frame);
                continue;
            }
            av_channel_layout_uninit(&out_layout);
            swr_src_fmt_ = frame->format;
            swr_src_rate_ = frame->sample_rate;
            swr_src_channels_ = channels;
        }

        AudioFrame af;
        af.sample_rate = frame->sample_rate;
        af.channels = channels;
        af.pts = frame->pts;
        af.data = std::make_unique<uint8_t[]>(
            static_cast<size_t>(frame->nb_samples) * channels * 2);

        uint8_t* out[1] = { af.data.get() };
        int converted = swr_convert(swr_ctx_, out, frame->nb_samples,
                                    const_cast<const uint8_t**>(frame->extended_data),
                                    frame->nb_samples);
        if (converted > 0) {
            af.size = converted * channels * 2;
            if (on_audio_frame_) on_audio_frame_(std::move(af));
        }

        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    return true;
}

void Decoder::flush() {
    if (video_ctx_) {
        std::lock_guard lock(video_mutex_);
        avcodec_flush_buffers(video_ctx_);
    }
    if (audio_ctx_) {
        std::lock_guard lock(audio_mutex_);
        avcodec_flush_buffers(audio_ctx_);
    }
}

} // namespace opm::media
