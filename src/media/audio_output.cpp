#include <opm/media/audio_output.h>
#include <cstring>
#include <iostream>

#include <SDL.h>

namespace opm::media {

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() {
    shutdown();
}

bool AudioOutput::init(int sample_rate, int channels) {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::cerr << "[Audio] SDL audio init failed: " << SDL_GetError() << "\n";
        return false;
    }

    SDL_AudioSpec desired{};
    desired.freq = sample_rate;
    desired.format = AUDIO_S16SYS;
    desired.channels = static_cast<uint8_t>(channels);
    desired.samples = 1024;
    desired.callback = AudioOutput::audio_callback;
    desired.userdata = this;

    SDL_AudioSpec obtained{};
    device_id_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (device_id_ == 0) {
        std::cerr << "[Audio] Failed to open audio device: " << SDL_GetError() << "\n";
        return false;
    }

    // Start playback
    SDL_PauseAudioDevice(device_id_, 0);
    initialized_.store(true);

    std::cout << "[Audio] Initialized: " << obtained.freq << "Hz, "
              << static_cast<int>(obtained.channels) << "ch\n";
    return true;
}

void AudioOutput::shutdown() {
    if (device_id_ != 0) {
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    initialized_.store(false);
}

void AudioOutput::submit(AudioFrame frame) {
    std::lock_guard lock(queue_mutex_);
    queue_.push(std::move(frame));

    // Prevent unbounded growth
    while (queue_.size() > 100) {
        queue_.pop();
    }
}

void AudioOutput::pause() {
    if (device_id_) SDL_PauseAudioDevice(device_id_, 1);
}

void AudioOutput::resume() {
    if (device_id_) SDL_PauseAudioDevice(device_id_, 0);
}

void AudioOutput::audio_callback(void* userdata, uint8_t* stream, int len) {
    auto* self = static_cast<AudioOutput*>(userdata);
    int written = 0;

    std::lock_guard lock(self->queue_mutex_);

    while (written < len && !self->queue_.empty()) {
        auto& front = self->queue_.front();
        int available = front.size - self->read_offset_;
        int to_copy = std::min(available, len - written);

        std::memcpy(stream + written, front.data.get() + self->read_offset_, to_copy);
        written += to_copy;
        self->read_offset_ += to_copy;

        if (self->read_offset_ >= front.size) {
            self->queue_.pop();
            self->read_offset_ = 0;
        }
    }

    // Fill remaining with silence
    if (written < len) {
        std::memset(stream + written, 0, len - written);
    }
}

} // namespace opm::media
