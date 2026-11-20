#include "ui/GifCaptureSession.h"
#include "media/GifEncoder.h"
#include "state/StateDocument.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace cd::plugin {
namespace {
constexpr std::size_t kOutputCap = 32u * 1024u * 1024u;
constexpr std::size_t kBufferMargin = 1024u * 1024u;
constexpr std::uint32_t kQueueSlots = 3;
constexpr unsigned kFps = 20;
constexpr std::chrono::milliseconds kFramePeriod{50};
constexpr std::size_t kDiagnosticBytes = 2048;

std::uint32_t frameLimit(double seconds) noexcept {
    return std::isfinite(seconds) && seconds >= 1.0 && seconds <= 15.0
        ? std::min<std::uint32_t>(300, static_cast<std::uint32_t>(std::ceil(seconds * kFps))) : 0;
}
} // namespace

std::size_t GifCaptureSession::bufferBudgetBytes(CaptureFraming framing) noexcept {
    const auto dimensions = captureDimensions(CaptureFormat::Gif, framing);
    if (dimensions.width <= 0 || dimensions.height <= 0) return std::numeric_limits<std::size_t>::max();
    const auto width = static_cast<std::uint32_t>(dimensions.width);
    const auto height = static_cast<std::uint32_t>(dimensions.height);
    const auto encoding = media::gifEncodingMemoryBound(width, height, kOutputCap);
    // Three queue slots, one producer conversion, one worker copy and three
    // software-render/readback allowances. No retained last-RGBA or output copy.
    const std::size_t external = std::size_t(width) * height * 4u * 8u + kBufferMargin;
    if (encoding > std::numeric_limits<std::size_t>::max() - external)
        return std::numeric_limits<std::size_t>::max();
    return encoding + external;
}

GifCaptureSession::GifCaptureSession(const CaptureRequest& request, std::uint64_t epoch,
                                     Clock clock, TestHooks hooks)
    : request_(request), epoch_(epoch),
      width_(static_cast<std::uint32_t>(captureDimensions(CaptureFormat::Gif, request.framing).width)),
      height_(static_cast<std::uint32_t>(captureDimensions(CaptureFormat::Gif, request.framing).height)),
      maxFrames_(frameLimit(request.seconds)), frameBytes_(std::size_t(width_) * height_ * 4),
      clock_(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })),
      hooks_(std::move(hooks)) {
    if (request_.format != CaptureFormat::Gif || request_.includeAudio || width_ == 0 || height_ == 0 ||
        maxFrames_ == 0 || request_.destination.getFullPathName().isEmpty() ||
        !request_.destination.hasFileExtension(".gif")) {
        startError_ = "Select a .gif destination, valid framing and 1–15 seconds without audio";
        state_.store(CaptureState::Failed, std::memory_order_release);
        return;
    }
    if (bufferBudgetBytes(request_.framing) > kBufferLimitBytes) {
        startError_ = "GIF capture exceeds the 64 MiB buffer budget";
        state_.store(CaptureState::Failed, std::memory_order_release);
        return;
    }
    destinationPath_ = request_.destination.getFullPathName().toStdString();
    media::CaptureQueueConfig config;
    config.slotCount = kQueueSlots;
    config.slotBytes = frameBytes_;
    config.memoryCapBytes = std::size_t(kQueueSlots) * frameBytes_;
    config.policy = media::OverflowPolicy::DropOldest;
    queue_ = std::make_unique<media::CaptureQueue>(config);
    if (!queue_->valid()) {
        startError_ = queue_->error();
        queue_.reset();
        state_.store(CaptureState::Failed, std::memory_order_release);
        return;
    }
    producerPixels_ = std::make_unique<std::uint8_t[]>(frameBytes_);
    workerPixels_ = std::make_unique<std::uint8_t[]>(frameBytes_);
    duration_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(request_.seconds));
    // The clock starts after admission and producer/queue allocation. Every
    // shared resource is initialized before the worker can observe this object.
    started_ = clock_();
    state_.store(CaptureState::Recording, std::memory_order_release);
    setMessage("Recording silent GIF", CaptureState::Recording);
    worker_ = std::thread([this] {
        run();
        workerFinished_.store(true, std::memory_order_release);
    });
}

GifCaptureSession::~GifCaptureSession() {
    stop();
    if (worker_.joinable()) worker_.join();
}

GifCaptureSession::ProducerGuard::ProducerGuard(GifCaptureSession& session) noexcept : session_(session) {
    bool expected = false;
    owned_ = session_.producerActive_.compare_exchange_strong(expected, true, std::memory_order_seq_cst);
}
GifCaptureSession::ProducerGuard::~ProducerGuard() {
    if (owned_) {
        // Publication and submittedHighWater precede this release. The worker
        // observes this gate before its final queue-empty observation.
        session_.producerActive_.store(false, std::memory_order_seq_cst);
        session_.wake_.notify_one();
    }
}

bool GifCaptureSession::deadlineReached(TimePoint now) const noexcept {
    return now >= started_ && now - started_ >= duration_;
}

std::uint32_t GifCaptureSession::elapsedFrames(TimePoint now, bool roundUp) const noexcept {
    if (now <= started_) return 0;
    if (deadlineReached(now)) return maxFrames_;
    const auto elapsed = now - started_;
    const auto whole = elapsed / kFramePeriod;
    const auto partial = roundUp && elapsed % kFramePeriod != std::chrono::steady_clock::duration::zero() ? 1 : 0;
    return std::min(maxFrames_, static_cast<std::uint32_t>(whole + partial));
}

std::optional<CaptureFrameRequest> GifCaptureSession::takeFrameRequest() noexcept {
    if (!ready()) return std::nullopt;
    ProducerGuard producer(*this);
    if (!producer || stopping_.load(std::memory_order_seq_cst) ||
        state_.load(std::memory_order_acquire) != CaptureState::Recording) return std::nullopt;
    try {
        const auto now = clock_();
        if (deadlineReached(now)) {
            beginStop(maxFrames_);
            return std::nullopt;
        }
        const auto index = elapsedFrames(now, false);
        if (index >= maxFrames_ || (lastRequested_ && index <= *lastRequested_)) return std::nullopt;
        lastRequested_ = index;
        return CaptureFrameRequest{epoch_, index, static_cast<int>(width_), static_cast<int>(height_), request_.framing};
    } catch (...) {
        fail(Failure::ProducerException);
        return std::nullopt;
    }
}

void GifCaptureSession::submit(const juce::Image& image) noexcept {
    if (const auto request = takeFrameRequest()) submit(*request, image);
}

void GifCaptureSession::submit(const CaptureFrameRequest& request, const juce::Image& image) noexcept {
    if (!ready()) return;
    ProducerGuard producer(*this);
    if (!producer || stopping_.load(std::memory_order_seq_cst) ||
        state_.load(std::memory_order_acquire) != CaptureState::Recording) return;
    // A token issued before finalization but submitted after it starts is ignored.
    // Holding a token alone cannot keep a disappeared editor's worker alive.
    if (request.epoch != epoch_ || !lastRequested_ || request.index != *lastRequested_ ||
        request.index >= maxFrames_ || (lastSubmitted_ && request.index <= *lastSubmitted_)) return;
    if (request.width != static_cast<int>(width_) || request.height != static_cast<int>(height_) ||
        request.framing != request_.framing || !image.isValid() ||
        image.getWidth() != static_cast<int>(width_) || image.getHeight() != static_cast<int>(height_)) {
        fail(Failure::InvalidFrame);
        return;
    }
    try {
        juce::Image::BitmapData pixels(image, juce::Image::BitmapData::readOnly);
        for (std::uint32_t y = 0; y < height_; ++y) {
            for (std::uint32_t x = 0; x < width_; ++x) {
                const auto colour = pixels.getPixelColour(static_cast<int>(x), static_cast<int>(y));
                const auto offset = (std::size_t(y) * width_ + x) * 4;
                producerPixels_[offset] = colour.getRed();
                producerPixels_[offset + 1] = colour.getGreen();
                producerPixels_[offset + 2] = colour.getBlue();
                producerPixels_[offset + 3] = 255; // GIF is opaque and silent.
            }
        }
        if (hooks_.beforeFramePublication) hooks_.beforeFramePublication();
        // This silent session uses the queue's integer PTS storage in 20 Hz
        // ticks. Sequence numbers describe storage only, never elapsed time.
        if (!queue_->push(producerPixels_.get(), frameBytes_, width_, height_, request.index)) {
            fail(Failure::QueueRefused);
            return;
        }
        lastSubmitted_ = request.index;
        submittedHighWater_.store(request.index + 1, std::memory_order_release);
        wake_.notify_one();
    } catch (...) {
        // No allocation is performed while constructing this failure reason.
        fail(Failure::ProducerException);
    }
}

void GifCaptureSession::beginStop(std::uint32_t frames) noexcept {
    CaptureState expected = CaptureState::Recording;
    if (state_.compare_exchange_strong(expected, CaptureState::Finalizing, std::memory_order_acq_rel)) {
        finalFrames_.store(std::min(frames, maxFrames_), std::memory_order_release);
        // Close admission only after publishing the winning (manual/deadline)
        // endpoint. An already admitted producer may finish its publication.
        stopping_.store(true, std::memory_order_seq_cst);
        setMessage("Finalizing silent GIF", CaptureState::Finalizing);
        wake_.notify_one();
    }
}

void GifCaptureSession::stop() noexcept {
    if (state_.load(std::memory_order_acquire) != CaptureState::Recording) return;
    auto frames = submittedHighWater_.load(std::memory_order_acquire);
    try { frames = std::max(frames, elapsedFrames(clock_(), true)); } catch (...) {}
    // Manual stop includes elapsed slots and every admitted final image, with
    // at most one 50-ms rounding slot. It never pads to the requested duration.
    beginStop(frames);
}

void GifCaptureSession::setMessage(std::string_view text, CaptureState expected) noexcept {
    try {
        std::lock_guard<std::mutex> lock(messageMutex_);
        const auto state = state_.load(std::memory_order_acquire);
        if (state != expected) return;
        message_.assign(text.data(), std::min(text.size(), kDiagnosticBytes));
        messageState_ = state;
    } catch (...) {}
}

const char* GifCaptureSession::failureMessage(Failure reason) noexcept {
    switch (reason) {
        case Failure::InvalidFrame: return "Capture frame dimensions changed or the editor snapshot was invalid; destination unchanged";
        case Failure::QueueRefused: return "GIF capture queue refused a frame; destination unchanged";
        case Failure::OutOfOrder: return "Out-of-order GIF capture frame; destination unchanged";
        case Failure::EncoderStart: return "Cannot initialize GIF encoder; destination unchanged";
        case Failure::FrameEncoding: return "GIF frame encoding failed; destination unchanged";
        case Failure::RepeatEncoding: return "GIF repeat encoding failed; destination unchanged";
        case Failure::NoFrames: return "No editor frames were captured; destination unchanged";
        case Failure::Finalization: return "GIF finalization failed; destination unchanged";
        case Failure::Save: return "Cannot save GIF; destination unchanged";
        case Failure::ProducerException: return "Cannot submit GIF frame; destination unchanged";
        case Failure::None:
        case Failure::WorkerException: return "GIF capture worker failed; destination unchanged";
    }
    return "GIF capture failed; destination unchanged";
}

void GifCaptureSession::fail(Failure reason, std::string_view detail) noexcept {
    if (state_.load(std::memory_order_acquire) == CaptureState::Completed) return;
    Failure expected = Failure::None;
    if (!failure_.compare_exchange_strong(expected, reason, std::memory_order_acq_rel)) return;
    auto previous = state_.load(std::memory_order_acquire);
    do {
        if (previous == CaptureState::Completed || previous == CaptureState::Failed) return;
    } while (!state_.compare_exchange_weak(previous, CaptureState::Failed, std::memory_order_acq_rel));
    aborted_.store(true, std::memory_order_release);
    stopping_.store(true, std::memory_order_seq_cst);
    if (queue_) queue_->fail();
    // The terminal reason exists independently of diagnostic allocations. A
    // bad_alloc here or in a catch handler can never escape this boundary.
    try {
        std::lock_guard<std::mutex> lock(messageMutex_);
        message_.assign(failureMessage(reason));
        if (!detail.empty()) {
            message_.append(": ");
            message_.append(detail.data(), std::min(detail.size(), kDiagnosticBytes));
        }
        messageState_ = CaptureState::Failed;
    } catch (...) {}
    wake_.notify_one();
}

void GifCaptureSession::run() noexcept {
    try {
        if (aborted_.load(std::memory_order_acquire)) return;
        media::GifWriter writer;
        media::GifOptions options;
        options.delayCentiseconds = 5;
        options.loop = request_.loop;
        media::GifLimits limits;
        limits.maxDimension = 1920;
        limits.maxFrames = maxFrames_;
        limits.maxBytes = kOutputCap;
        std::string error;
        if (!writer.begin(width_, height_, options, limits, error)) {
            fail(Failure::EncoderStart, error);
            return;
        }
        const auto repeat = [&] {
            if (!writer.repeatLastFrame(error)) {
                fail(Failure::RepeatEncoding, error);
                return false;
            }
            repeated_.fetch_add(1, std::memory_order_relaxed);
            written_.store(writer.frameCount(), std::memory_order_release);
            return true;
        };
        for (;;) {
            if (aborted_.load(std::memory_order_acquire)) return;
            if (state_.load(std::memory_order_acquire) == CaptureState::Recording && deadlineReached(clock_()))
                beginStop(maxFrames_);

            // These observations MUST precede acquire(). If stop becomes true
            // after an empty acquire, another pass must drain its publication.
            // The SC admission/gate order also prevents a producer from entering
            // after a stopped+inactive observation and publishing unseen data.
            const bool stopped = stopping_.load(std::memory_order_seq_cst);
            const bool producerWasActive = producerActive_.load(std::memory_order_seq_cst);
            media::CaptureFrame frame;
            if (queue_->acquire(frame)) {
                const auto index = frame.ptsSamples;
                const bool valid = frame.bytes == frameBytes_ && frame.width == width_ && frame.height == height_ &&
                    index >= 0 && index < maxFrames_ && static_cast<std::uint32_t>(index) >= writer.frameCount();
                if (valid) std::memcpy(workerPixels_.get(), frame.data, frameBytes_);
                queue_->release(frame); // No queue slot is retained during encoding or hooks.
                if (!valid) { fail(Failure::OutOfOrder); return; }
                if (hooks_.beforeFrameEncoding) hooks_.beforeFrameEncoding();
                if (writer.frameCount() == 0) {
                    // If capture started late, its first real visual fills the
                    // missing prefix too; no black or fabricated frame is added.
                    if (!writer.addFrame(workerPixels_.get(), frameBytes_, error)) {
                        fail(Failure::FrameEncoding, error);
                        return;
                    }
                    written_.store(writer.frameCount(), std::memory_order_release);
                    while (writer.frameCount() <= static_cast<std::uint32_t>(index))
                        if (!repeat()) return;
                } else {
                    while (writer.frameCount() < static_cast<std::uint32_t>(index))
                        if (!repeat()) return;
                    if (!writer.addFrame(workerPixels_.get(), frameBytes_, error)) {
                        fail(Failure::FrameEncoding, error);
                        return;
                    }
                    written_.store(writer.frameCount(), std::memory_order_release);
                }
                continue;
            }
            if (hooks_.afterEmptyAcquire) hooks_.afterEmptyAcquire();
            if (stopped && !producerWasActive) break;
            std::unique_lock<std::mutex> lock(wakeMutex_);
            // Bounded independently of visible panels, editor timer ticks or
            // notification delivery. Injected-clock tests can wake immediately.
            wake_.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (aborted_.load(std::memory_order_acquire)) return;
        if (writer.frameCount() == 0) { fail(Failure::NoFrames); return; }
        const auto target = std::min(maxFrames_, std::max(finalFrames_.load(std::memory_order_acquire),
                                                        submittedHighWater_.load(std::memory_order_acquire)));
        while (writer.frameCount() < target)
            if (!repeat()) return;
        if (aborted_.load(std::memory_order_acquire)) return;
        std::vector<std::uint8_t> encoded;
        if (!writer.finish(encoded, error)) { fail(Failure::Finalization, error); return; }
        const std::string_view bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        if (!state::saveBytesFileAtomic(destinationPath_, bytes, error)) {
            fail(Failure::Save, error);
            return;
        }
        // Atomic installation is the success point. Later descriptive allocation
        // or test-hook failure cannot relabel a committed file as failed.
        state_.store(CaptureState::Completed, std::memory_order_release);
        try {
            if (hooks_.beforeCompletionMessage) hooks_.beforeCompletionMessage();
            const auto message = std::string("Saved ") + destinationPath_;
            setMessage(message, CaptureState::Completed);
        } catch (...) {}
    } catch (...) {
        fail(Failure::WorkerException);
    }
}

CaptureStatus GifCaptureSession::status() const {
    CaptureStatus status;
    status.state = state_.load(std::memory_order_acquire);
    status.framesWritten = static_cast<int>(written_.load(std::memory_order_acquire));
    status.framesRepeated = static_cast<int>(repeated_.load(std::memory_order_acquire));
    if (status.state == CaptureState::Completed) status.progress = 1.0;
    else if (maxFrames_ != 0 && status.state == CaptureState::Finalizing)
        status.progress = std::min(0.99, 0.9 + 0.09 * double(status.framesWritten) / double(maxFrames_));
    else if (maxFrames_ != 0)
        status.progress = std::min(0.9, 0.9 * double(submittedHighWater_.load(std::memory_order_acquire)) / double(maxFrames_));
    {
        std::lock_guard<std::mutex> lock(messageMutex_);
        if (messageState_ == status.state && !message_.empty()) status.message = juce::String::fromUTF8(message_.c_str());
        else if (status.state == CaptureState::Failed)
            status.message = juce::String::fromUTF8(startError_.empty() ? failureMessage(failure_.load(std::memory_order_acquire))
                                                                     : startError_.c_str());
        else if (status.state == CaptureState::Completed) status.message = "Saved silent GIF";
        else if (status.state == CaptureState::Finalizing) status.message = "Finalizing silent GIF";
        else if (status.state == CaptureState::Recording) status.message = "Recording silent GIF";
    }
    status.mp4Reason = "Native MP4 encoder not available (G07)";
    return status;
}

} // namespace cd::plugin
