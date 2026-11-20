// Message-thread producer and independent worker for bounded, silent GIF capture.
// The immutable request and monotonic 20 Hz clock belong to this session, not a
// visible panel. Nothing here is used by the audio thread.
#pragma once
#include "ui/EditorBridge.h"
#include "media/CaptureQueue.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace cd::plugin {

class GifCaptureSession final {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    // Injected clocks must be monotonic and safe to read from both threads.
    using Clock = std::function<TimePoint()>;
    // Deterministic native regression seams. Production leaves every hook empty.
    // Hooks may throw; worker/producer boundaries contain those exceptions.
    struct TestHooks {
        std::function<void()> afterEmptyAcquire;
        std::function<void()> beforeFramePublication;
        std::function<void()> beforeFrameEncoding;
        std::function<void()> beforeCompletionMessage;
    };

    explicit GifCaptureSession(const CaptureRequest&, std::uint64_t epoch = 0,
                               Clock clock = {}, TestHooks hooks = {});
    ~GifCaptureSession();
    GifCaptureSession(const GifCaptureSession&) = delete;
    GifCaptureSession& operator=(const GifCaptureSession&) = delete;

    bool ready() const noexcept { return queue_ != nullptr && worker_.joinable(); }
    CaptureState captureState() const noexcept { return state_.load(std::memory_order_acquire); }
    // Terminal state precedes best-effort diagnostic cleanup after installation.
    bool workerFinished() const noexcept { return workerFinished_.load(std::memory_order_acquire); }
    const std::string& startError() const noexcept { return startError_; }
    std::optional<CaptureFrameRequest> takeFrameRequest() noexcept;
    void submit(const CaptureFrameRequest&, const juce::Image&) noexcept;
    // Compatibility entry point uses the same clock/request admission, never a
    // second frame counter that would accelerate rapidly submitted snapshots.
    void submit(const juce::Image&) noexcept;
    void stop() noexcept;
    CaptureStatus status() const;
    void notifyClockAdvanced() noexcept { wake_.notify_one(); }

    // Explicit buffer admission, not a whole-process/RSS or GPU-memory claim.
    static constexpr std::size_t kBufferLimitBytes = 64u * 1024u * 1024u;
    static std::size_t bufferBudgetBytes(CaptureFraming) noexcept;

private:
    enum class Failure : unsigned char {
        None, InvalidFrame, QueueRefused, OutOfOrder, EncoderStart, FrameEncoding,
        RepeatEncoding, NoFrames, Finalization, Save, ProducerException, WorkerException
    };
    class ProducerGuard {
    public:
        explicit ProducerGuard(GifCaptureSession&) noexcept;
        ~ProducerGuard();
        explicit operator bool() const noexcept { return owned_; }
    private:
        GifCaptureSession& session_;
        bool owned_ = false;
    };

    std::uint32_t elapsedFrames(TimePoint now, bool roundUp) const noexcept;
    bool deadlineReached(TimePoint now) const noexcept;
    void beginStop(std::uint32_t frames) noexcept;
    void run() noexcept;
    void fail(Failure, std::string_view detail = {}) noexcept;
    void setMessage(std::string_view, CaptureState expected) noexcept;
    static const char* failureMessage(Failure) noexcept;

    const CaptureRequest request_;
    const std::uint64_t epoch_;
    const std::uint32_t width_, height_, maxFrames_;
    const std::size_t frameBytes_;
    const Clock clock_;
    const TestHooks hooks_;
    TimePoint started_{};
    std::chrono::steady_clock::duration duration_{};
    std::string destinationPath_;
    std::unique_ptr<media::CaptureQueue> queue_;
    std::unique_ptr<std::uint8_t[]> producerPixels_, workerPixels_;
    std::thread worker_;
    std::string startError_;
    mutable std::mutex messageMutex_;
    std::string message_;
    CaptureState messageState_ = CaptureState::Idle; // guarded by messageMutex_
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    std::atomic<CaptureState> state_{CaptureState::Idle};
    std::atomic<Failure> failure_{Failure::None};
    std::atomic<bool> stopping_{false}, aborted_{false}, producerActive_{false}, workerFinished_{false};
    std::atomic<std::uint32_t> finalFrames_{0}, submittedHighWater_{0}, written_{0}, repeated_{0};
    // Message-thread-only token bookkeeping; the worker reads only queue PTS.
    std::optional<std::uint32_t> lastRequested_, lastSubmitted_;
};

} // namespace cd::plugin
