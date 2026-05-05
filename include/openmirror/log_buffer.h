#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <thread>

namespace openmirror {

// Thread-safe ring buffer that captures std::cout output
class LogBuffer {
public:
    static LogBuffer& instance() {
        static LogBuffer buf;
        return buf;
    }

    void add_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
        if (lines_.size() > max_lines_) lines_.pop_front();
        ++version_;
    }

    std::deque<std::string> get_lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.size();
    }

    uint64_t version() const { return version_.load(); }

    // Install as capture on std::cout and std::cerr
    void install() {
        original_buf_ = std::cout.rdbuf();
        tee_buf_ = std::make_unique<TeeBuf>(this);
        std::cout.rdbuf(tee_buf_.get());

        original_cerr_buf_ = std::cerr.rdbuf();
        cerr_tee_buf_ = std::make_unique<TeeBuf>(this);
        std::cerr.rdbuf(cerr_tee_buf_.get());
    }

    void uninstall() {
        if (original_buf_) {
            std::cout.rdbuf(original_buf_);
            original_buf_ = nullptr;
        }
        if (original_cerr_buf_) {
            std::cerr.rdbuf(original_cerr_buf_);
            original_cerr_buf_ = nullptr;
        }
    }

private:
    LogBuffer() = default;
    ~LogBuffer() { uninstall(); }
    LogBuffer(const LogBuffer&) = delete;
    LogBuffer& operator=(const LogBuffer&) = delete;

    class TeeBuf : public std::streambuf {
    public:
        TeeBuf(LogBuffer* log) : log_(log) {}

    protected:
        int overflow(int c) override {
            std::lock_guard<std::mutex> lock(buf_mutex_);
            auto tid = std::this_thread::get_id();
            auto& buf = thread_bufs_[tid];
            if (c == '\n') {
                log_->add_line(buf);
                buf.clear();
            } else if (c != EOF && c != '\r') {
                buf += static_cast<char>(c);
            }
            return c;
        }

        std::streamsize xsputn(const char* s, std::streamsize n) override {
            std::lock_guard<std::mutex> lock(buf_mutex_);
            auto tid = std::this_thread::get_id();
            auto& buf = thread_bufs_[tid];
            for (std::streamsize i = 0; i < n; ++i) {
                char c = s[i];
                if (c == '\n') {
                    log_->add_line(buf);
                    buf.clear();
                } else if (c != '\r') {
                    buf += c;
                }
            }
            return n;
        }

        int sync() override { return 0; }

    private:
        LogBuffer* log_;
        std::mutex buf_mutex_;
        std::map<std::thread::id, std::string> thread_bufs_;
    };

    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    std::atomic<uint64_t> version_{0};
    size_t max_lines_ = 500;
    std::streambuf* original_buf_ = nullptr;
    std::unique_ptr<TeeBuf> tee_buf_;
    std::streambuf* original_cerr_buf_ = nullptr;
    std::unique_ptr<TeeBuf> cerr_tee_buf_;
};

} // namespace openmirror
