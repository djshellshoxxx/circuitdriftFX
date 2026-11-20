// Silent, bounded local GIF capture service for the production editor (G11 partial).
// Message thread owns start/submit/stop/status; a dedicated worker owns encoding and disk I/O.
// Neither the audio callback nor the rendering thread waits for the encoder. The SPSC queue
// is fixed at three RGBA frames; dropped ready frames repeat the last encoded frame so
// output duration tracks accepted captures rather than silently becoming shorter.
#pragma once

#include "ui/EditorBridge.h"
#include "media/CaptureQueue.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace cd::plugin {

class GifCaptureSession final {
public:
    explicit GifCaptureSession(const CaptureRequest& request);
    ~GifCaptureSession();
    GifCaptureSession(const GifCaptureSession&) = delete;
    GifCaptureSession& operator=(const GifCaptureSession&) = delete;

    bool ready() const noexcept { return queue_ != nullptr && worker_.joinable(); }
    const std::string& startError() const noexcept { return startError_; }
    void submit(const juce::Image& image); // message thread: image -> fixed-size RGBA -> SPSC queue
    void stop() noexcept;                  // nonblocking; worker drains and writes atomically
    CaptureStatus status() const;          // message thread, no audio ownership

private:
    void run() noexcept;
    void fail(const std::string& reason) noexcept;
    void setMessage(const std::string& message);
    static std::pair<std::uint32_t, std::uint32_t> dimensions(CaptureFraming framing) noexcept;

    const CaptureRequest request_;
    const std::uint32_t width_, height_, maxFrames_;
    std::unique_ptr<media::CaptureQueue> queue_;
    std::thread worker_;
    std::string startError_;
    mutable std::mutex messageMutex_;
    std::string message_ = "Recording silent GIF";
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    std::atomic<CaptureState> state_{CaptureState::Recording};
    std::atomic<bool> stopping_{false}, aborted_{false};
    std::atomic<unsigned> submitted_{0}, written_{0}, repeated_{0};
};

} // namespace cd::plugin
