#include "ui/GifCaptureSession.h"
#include "media/GifEncoder.h"
#include "state/StateDocument.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <vector>

namespace cd::plugin {
namespace {
constexpr std::size_t kOutputCap = 32u * 1024u * 1024u;
constexpr std::size_t kQueueCap = 16u * 1024u * 1024u;
constexpr unsigned kFps = 20;

std::string localPath(const juce::File& file) {
    // Match the existing atomic PNG writer's JUCE-to-UTF-8 path contract.
    return file.getFullPathName().toStdString();
}
} // namespace

std::pair<std::uint32_t, std::uint32_t> GifCaptureSession::dimensions(CaptureFraming framing) noexcept {
    switch (framing) {
        case CaptureFraming::Portrait9x16: return {360, 640};
        case CaptureFraming::Square1x1: return {480, 480};
        case CaptureFraming::Landscape16x9: return {640, 360};
    }
    return {0, 0};
}

GifCaptureSession::GifCaptureSession(const CaptureRequest& request)
    : request_(request),
      width_(dimensions(request.framing).first),
      height_(dimensions(request.framing).second),
      maxFrames_(std::isfinite(request.seconds) && request.seconds >= 1.0 && request.seconds <= 15.0
                     ? std::min<std::uint32_t>(300, static_cast<std::uint32_t>(std::ceil(request.seconds * kFps)))
                     : 0) {
    if (request.format != CaptureFormat::Gif || request.includeAudio || width_ == 0 || height_ == 0 ||
        maxFrames_ == 0 || localPath(request.destination).empty() ||
        !request.destination.hasFileExtension(".gif")) {
        startError_ = "Select a .gif destination, valid framing and 1–15 seconds without audio";
        return;
    }
    media::CaptureQueueConfig cfg;
    cfg.slotCount = 3;
    cfg.slotBytes = static_cast<std::size_t>(width_) * height_ * 4;
    cfg.memoryCapBytes = kQueueCap;
    cfg.policy = media::OverflowPolicy::DropOldest;
    queue_ = std::make_unique<media::CaptureQueue>(cfg);
    if (!queue_->valid()) {
        startError_ = queue_->error();
        queue_.reset();
        return;
    }
    // All queue resources are constructed before the worker can observe them.
    worker_ = std::thread([this] { run(); });
}

GifCaptureSession::~GifCaptureSession() {
    stop();
    if (worker_.joinable()) worker_.join();
}

void GifCaptureSession::setMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(messageMutex_);
    message_ = message;
}

void GifCaptureSession::fail(const std::string& reason) noexcept {
    aborted_.store(true, std::memory_order_release);
    stopping_.store(true, std::memory_order_release);
    try { setMessage(reason); } catch (...) {}
    state_.store(CaptureState::Failed, std::memory_order_release);
    wake_.notify_one();
}

void GifCaptureSession::submit(const juce::Image& image) {
    if (!ready() || state_.load(std::memory_order_acquire) != CaptureState::Recording) return;
    if (!image.isValid() || image.getWidth() != static_cast<int>(width_) ||
        image.getHeight() != static_cast<int>(height_)) {
        fail("Capture frame dimensions changed or the editor snapshot was invalid; destination unchanged");
        return;
    }
    const unsigned current = submitted_.load(std::memory_order_relaxed);
    if (current >= maxFrames_) { stop(); return; }
    try {
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width_) * height_ * 4);
        juce::Image::BitmapData pixels(image, juce::Image::BitmapData::readOnly);
        for (unsigned y = 0; y < height_; ++y) {
            for (unsigned x = 0; x < width_; ++x) {
                const auto colour = pixels.getPixelColour(static_cast<int>(x), static_cast<int>(y));
                const std::size_t offset = (static_cast<std::size_t>(y) * width_ + x) * 4;
                rgba[offset] = colour.getRed();
                rgba[offset + 1] = colour.getGreen();
                rgba[offset + 2] = colour.getBlue();
                rgba[offset + 3] = 255; // GIF is opaque; ignore editor alpha
            }
        }
        if (!queue_->push(rgba.data(), rgba.size(), width_, height_, 0)) {
            fail("GIF capture queue refused a frame; destination unchanged");
            return;
        }
        submitted_.store(current + 1, std::memory_order_release);
        wake_.notify_one();
        if (current + 1 == maxFrames_) stop();
    } catch (const std::exception& e) {
        fail(std::string("Cannot submit GIF frame: ") + e.what());
    } catch (...) {
        fail("Cannot submit GIF frame");
    }
}

void GifCaptureSession::stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    CaptureState expected = CaptureState::Recording;
    if (state_.compare_exchange_strong(expected, CaptureState::Finalizing, std::memory_order_acq_rel)) {
        try { setMessage("Finalizing silent GIF"); } catch (...) {}
    }
    wake_.notify_one();
}

void GifCaptureSession::run() noexcept {
    try {
        media::GifWriter writer;
        media::GifOptions opts;
        opts.delayCentiseconds = 5; // 20 fps, silent
        opts.loop = request_.loop;
        media::GifLimits limits;
        limits.maxDimension = 1920;
        limits.maxFrames = maxFrames_;
        limits.maxBytes = kOutputCap;
        std::string error;
        if (!writer.begin(width_, height_, opts, limits, error)) {
            fail("Cannot initialize GIF encoder: " + error);
            return;
        }
        std::vector<std::uint8_t> last;
        std::uint64_t expectedSequence = 1;
        for (;;) {
            if (aborted_.load(std::memory_order_acquire)) return;
            media::CaptureFrame frame;
            if (queue_->acquire(frame)) {
                // Copy out and release the queue slot before the slow quantizer runs.
                std::vector<std::uint8_t> pixels(frame.data, frame.data + frame.bytes);
                const auto sequence = frame.sequence;
                queue_->release(frame);
                if (sequence < expectedSequence || sequence > maxFrames_) {
                    fail("Out-of-order GIF capture frame");
                    return;
                }
                // The bounded queue prefers current visuals. Repeating the last
                // successfully encoded frame preserves the GIF's 50-ms timebase.
                while (expectedSequence < sequence) {
                    const auto& repeated = last.empty() ? pixels : last;
                    if (!writer.addFrame(repeated.data(), repeated.size(), error)) {
                        fail("GIF repeat encoding failed: " + error);
                        return;
                    }
                    repeated_.fetch_add(1, std::memory_order_relaxed);
                    written_.store(writer.frameCount(), std::memory_order_release);
                    ++expectedSequence;
                }
                if (!writer.addFrame(pixels.data(), pixels.size(), error)) {
                    fail("GIF frame encoding failed: " + error);
                    return;
                }
                last.swap(pixels);
                expectedSequence = sequence + 1;
                written_.store(writer.frameCount(), std::memory_order_release);
                continue;
            }
            if (stopping_.load(std::memory_order_acquire)) break;
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(20));
        }
        // A final dropped-ready slot is still accounted for even if the queue
        // drained before it could be observed. Manual stop preserves shorter clips.
        const auto count = submitted_.load(std::memory_order_acquire);
        while (writer.frameCount() < count && !last.empty()) {
            if (!writer.addFrame(last.data(), last.size(), error)) {
                fail("GIF final-frame encoding failed: " + error);
                return;
            }
            repeated_.fetch_add(1, std::memory_order_relaxed);
            written_.store(writer.frameCount(), std::memory_order_release);
        }
        if (aborted_.load(std::memory_order_acquire)) return;
        if (writer.frameCount() == 0) { fail("No editor frames were captured; destination unchanged"); return; }
        std::vector<std::uint8_t> encoded;
        if (!writer.finish(encoded, error)) { fail("GIF finalization failed: " + error); return; }
        // The existing checked same-directory writer never truncates an old file
        // on encode/write failure. This work is strictly off the audio/message threads.
        const std::string bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        if (!state::saveStateFileAtomic(localPath(request_.destination), bytes, error)) {
            fail("Cannot save GIF: " + error);
            return;
        }
        setMessage("Saved " + localPath(request_.destination));
        state_.store(CaptureState::Completed, std::memory_order_release);
    } catch (const std::exception& e) {
        fail(std::string("GIF capture worker failed: ") + e.what());
    } catch (...) {
        fail("GIF capture worker failed");
    }
}

CaptureStatus GifCaptureSession::status() const {
    CaptureStatus s;
    s.state = state_.load(std::memory_order_acquire);
    s.framesWritten = static_cast<int>(written_.load(std::memory_order_acquire));
    s.framesRepeated = static_cast<int>(repeated_.load(std::memory_order_acquire));
    if (s.state == CaptureState::Completed) s.progress = 1.0;
    else if (s.state == CaptureState::Finalizing)
        s.progress = std::min(0.99, 0.9 + 0.09 * double(s.framesWritten) / double(maxFrames_));
    else s.progress = std::min(0.9, 0.9 * double(submitted_.load(std::memory_order_acquire)) / double(maxFrames_));
    {
        std::lock_guard<std::mutex> lock(messageMutex_);
        s.message = juce::String::fromUTF8(message_.c_str());
    }
    s.mp4Reason = "Native MP4 encoder not available (G07)";
    return s;
}

} // namespace cd::plugin
