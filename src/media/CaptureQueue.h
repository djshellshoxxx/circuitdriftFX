// CircuitDrift media: bounded capture queues (CD-F03 exporter, 06-interface spec).
// Frame queue: preallocated slot pool; producers never wait, allocate, lock or perform I/O.
//   push() is lock-free (atomic slot-state CAS, bounded scan of <= 2*slotCount slots) and noexcept.
//   Overflow policy: DropOldest (overwrite the oldest ready, unclaimed frame; counted) or Refuse
//   (drop the new frame; counted). A consumer-side failure (disk full, codec failure) latches the
//   queue into Failed: later pushes return false immediately and are counted. One producer
//   thread and one consumer thread are the supported topology; the state machine is CAS-safe.
// Memory: slotCount*slotBytes is checked against memoryCapBytes at construction (creation is
//   worker-side and may allocate); pushes of oversize frames are refused and counted.
// Audio ring: SPSC float ring, push never blocks; on overflow nothing is written, the overflow
//   flag latches and the owner stops capture and finalizes with an explicit error (spec).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cd::media {

enum class OverflowPolicy : unsigned char { DropOldest, Refuse };

struct CaptureQueueConfig {
    std::uint32_t slotCount = 3;                        // spec: three CPU readback buffers
    std::size_t slotBytes = 0;                          // capacity of one frame (e.g. w*h*4)
    std::size_t memoryCapBytes = 64u * 1024u * 1024u;   // spec: additional 64 MiB ceiling
    OverflowPolicy policy = OverflowPolicy::DropOldest;
};

struct CaptureFrame {                                   // consumer view; valid until release()
    const std::uint8_t* data = nullptr;
    std::size_t bytes = 0;
    std::uint32_t width = 0, height = 0;
    std::int64_t ptsSamples = 0;                        // audio-sample-index PTS
    std::uint64_t sequence = 0;
    std::uint32_t slot = 0;
};

struct CaptureQueueStats {
    std::uint64_t pushed = 0, droppedOldest = 0, refusedFull = 0, refusedOversize = 0, refusedFailed = 0,
                  consumed = 0, sinkStalls = 0;
};

class CaptureQueue {
public:
    // Worker-side. On failure valid() is false and error() says why.
    explicit CaptureQueue(const CaptureQueueConfig& cfg);
    bool valid() const noexcept { return valid_; }
    const std::string& error() const noexcept { return error_; }
    std::size_t memoryBytes() const noexcept { return std::size_t(cfg_.slotCount) * cfg_.slotBytes; }

    // Producer (render/capture side). Copies `bytes` into a slot. Returns true when stored.
    bool push(const std::uint8_t* data, std::size_t bytes, std::uint32_t width, std::uint32_t height,
              std::int64_t ptsSamples) noexcept;

    // Consumer (capture worker). acquire() claims the oldest ready frame; release() frees it.
    bool acquire(CaptureFrame& out) noexcept;
    void release(const CaptureFrame& f) noexcept;

    // Consumer-side latch: failed capture (disk full, codec error). Never blocks producers.
    void fail() noexcept { failed_.store(true, std::memory_order_release); }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    void noteSinkStall() noexcept { stalls_.fetch_add(1, std::memory_order_relaxed); }
    CaptureQueueStats stats() const noexcept;

private:
    struct Slot {
        std::atomic<std::uint32_t> state{0};  // 0 free, 1 writing, 2 ready, 3 reading
        std::atomic<std::uint64_t> seq{0};
        std::size_t bytes = 0;
        std::uint32_t width = 0, height = 0;
        std::int64_t pts = 0;
    };
    CaptureQueueConfig cfg_;
    bool valid_ = false;
    std::string error_;
    std::unique_ptr<std::uint8_t[]> storage_;
    std::unique_ptr<Slot[]> slots_;
    std::atomic<std::uint64_t> nextSeq_{1};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> pushed_{0}, droppedOldest_{0}, refusedFull_{0}, refusedOversize_{0},
        refusedFailed_{0}, consumed_{0}, stalls_{0};
};

enum class SinkStatus : unsigned char { Ok, Stalled, DiskFull, Failed };
class FrameSink {
public:
    virtual ~FrameSink() = default;
    virtual SinkStatus consume(const CaptureFrame& frame) = 0;  // worker thread
};
// Consumer helper: drains up to maxFrames. Stalled: frame is released (producer already drops by
// policy; the sink repeats its last frame). DiskFull/Failed: queue latches Failed and draining stops.
// Returns the number of frames the sink accepted.
std::size_t drainQueue(CaptureQueue& q, FrameSink& sink, std::size_t maxFrames);

class AudioCaptureRing {
public:
    AudioCaptureRing(std::size_t frameCapacity, unsigned channels);
    bool valid() const noexcept { return channels_ > 0 && !buf_.empty(); }
    std::size_t memoryBytes() const noexcept { return buf_.size() * sizeof(float); }
    // Audio thread: interleaved float frames. All-or-nothing; false => overflow latched.
    bool push(const float* interleaved, std::size_t frames) noexcept;
    // Worker: pops up to maxFrames; returns frames read.
    std::size_t pop(float* interleaved, std::size_t maxFrames) noexcept;
    bool overflowed() const noexcept { return overflow_.load(std::memory_order_acquire); }
    std::size_t available() const noexcept;
private:
    unsigned channels_;
    std::size_t capacity_;
    std::vector<float> buf_;
    std::atomic<std::uint64_t> head_{0}, tail_{0};  // frames written / read
    std::atomic<bool> overflow_{false};
};

// Budget helper for the capability report: CPU readback slots + audio ring.
std::size_t captureBudgetBytes(std::uint32_t slots, std::uint32_t width, std::uint32_t height,
                               double audioSeconds, double sampleRate, unsigned channels);

}  // namespace cd::media
