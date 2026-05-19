#pragma once

#include <opm/media/decoder.h>
#include <atomic>
#include <mutex>
#include <queue>

struct SDL_AudioSpec;

namespace opm::media {

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    bool init(int sample_rate, int channels);
    void shutdown();

    // Queue decoded audio for playback (thread-safe)
    void submit(AudioFrame frame);

    void pause();
    void resume();

private:
    static void audio_callback(void* userdata, uint8_t* stream, int len);

    uint32_t device_id_ = 0;
    std::mutex queue_mutex_;
    std::queue<AudioFrame> queue_;

    int read_offset_ = 0; // offset into current frame being read
    std::atomic<bool> initialized_{false};
};

} // namespace opm::media
