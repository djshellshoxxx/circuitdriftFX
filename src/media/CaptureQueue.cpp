#include "media/CaptureQueue.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace cd::media {
namespace {
constexpr std::uint32_t kFree = 0, kWriting = 1, kReady = 2, kReading = 3;
}

CaptureQueue::CaptureQueue(const CaptureQueueConfig& cfg) : cfg_(cfg) {
    if (cfg.slotCount == 0 || cfg.slotCount > 16) { error_ = "slotCount must be 1..16"; return; }
    if (cfg.slotBytes == 0) { error_ = "slotBytes must be non-zero"; return; }
    if (cfg.slotBytes > std::numeric_limits<std::size_t>::max() / cfg.slotCount) { error_ = "queue size overflows"; return; }
    if (cfg.slotBytes * cfg.slotCount > cfg.memoryCapBytes) { error_ = "capture queue exceeds memory cap"; return; }
    try {
        storage_.reset(new std::uint8_t[cfg.slotBytes * cfg.slotCount]);
        slots_.reset(new Slot[cfg.slotCount]);
    } catch (const std::bad_alloc&) { error_ = "out of memory"; return; }
    valid_ = true;
}

bool CaptureQueue::push(const std::uint8_t* data, std::size_t bytes, std::uint32_t width, std::uint32_t height,
                        std::int64_t ptsSamples) noexcept {
    if (!valid_) return false;
    if (failed_.load(std::memory_order_acquire)) { refusedFailed_.fetch_add(1, std::memory_order_relaxed); return false; }
    if (!data || bytes == 0 || bytes > cfg_.slotBytes) { refusedOversize_.fetch_add(1, std::memory_order_relaxed); return false; }
    std::uint32_t claimed = cfg_.slotCount;
    bool dropped = false;
    for (int pass = 0; pass < 2 && claimed == cfg_.slotCount; ++pass) {
        for (std::uint32_t i = 0; i < cfg_.slotCount; ++i) {
            std::uint32_t expect = kFree;
            if (slots_[i].state.compare_exchange_strong(expect, kWriting, std::memory_order_acquire)) { claimed = i; break; }
        }
        if (claimed != cfg_.slotCount || cfg_.policy == OverflowPolicy::Refuse) break;
        // Pool full: evict the oldest frame nobody is reading.
        std::uint32_t oldest = cfg_.slotCount;
        std::uint64_t oldestSeq = std::numeric_limits<std::uint64_t>::max();
        for (std::uint32_t i = 0; i < cfg_.slotCount; ++i) {
            if (slots_[i].state.load(std::memory_order_acquire) != kReady) continue;
            const auto s = slots_[i].seq.load(std::memory_order_relaxed);
            if (s < oldestSeq) { oldestSeq = s; oldest = i; }
        }
        if (oldest != cfg_.slotCount) {
            std::uint32_t expect = kReady;
            if (slots_[oldest].state.compare_exchange_strong(expect, kWriting, std::memory_order_acquire)) { claimed = oldest; dropped = true; }
        }
    }
    if (claimed == cfg_.slotCount) { refusedFull_.fetch_add(1, std::memory_order_relaxed); return false; }
    Slot& s = slots_[claimed];
    std::memcpy(storage_.get() + std::size_t(claimed) * cfg_.slotBytes, data, bytes);
    s.bytes = bytes; s.width = width; s.height = height; s.pts = ptsSamples;
    s.seq.store(nextSeq_.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    s.state.store(kReady, std::memory_order_release);
    pushed_.fetch_add(1, std::memory_order_relaxed);
    if (dropped) droppedOldest_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool CaptureQueue::acquire(CaptureFrame& out) noexcept {
    if (!valid_) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::uint32_t oldest = cfg_.slotCount;
        std::uint64_t oldestSeq = std::numeric_limits<std::uint64_t>::max();
        for (std::uint32_t i = 0; i < cfg_.slotCount; ++i) {
            if (slots_[i].state.load(std::memory_order_acquire) != kReady) continue;
            const auto s = slots_[i].seq.load(std::memory_order_relaxed);
            if (s < oldestSeq) { oldestSeq = s; oldest = i; }
        }
        if (oldest == cfg_.slotCount) return false;
        std::uint32_t expect = kReady;
        if (!slots_[oldest].state.compare_exchange_strong(expect, kReading, std::memory_order_acquire)) continue;  // evicted
        const Slot& s = slots_[oldest];
        out.data = storage_.get() + std::size_t(oldest) * cfg_.slotBytes;
        out.bytes = s.bytes; out.width = s.width; out.height = s.height; out.ptsSamples = s.pts;
        out.sequence = oldestSeq; out.slot = oldest;
        return true;
    }
    return false;
}

void CaptureQueue::release(const CaptureFrame& f) noexcept {
    if (!valid_ || f.slot >= cfg_.slotCount) return;
    consumed_.fetch_add(1, std::memory_order_relaxed);
    slots_[f.slot].state.store(kFree, std::memory_order_release);
}

CaptureQueueStats CaptureQueue::stats() const noexcept {
    CaptureQueueStats s;
    s.pushed = pushed_.load(); s.droppedOldest = droppedOldest_.load(); s.refusedFull = refusedFull_.load();
    s.refusedOversize = refusedOversize_.load(); s.refusedFailed = refusedFailed_.load();
    s.consumed = consumed_.load(); s.sinkStalls = stalls_.load();
    return s;
}

std::size_t drainQueue(CaptureQueue& q, FrameSink& sink, std::size_t maxFrames) {
    std::size_t accepted = 0;
    CaptureFrame f;
    for (std::size_t n = 0; n < maxFrames && q.acquire(f); ++n) {
        const SinkStatus st = sink.consume(f);
        q.release(f);
        if (st == SinkStatus::Ok) { ++accepted; continue; }
        if (st == SinkStatus::Stalled) { q.noteSinkStall(); continue; }
        q.fail();  // DiskFull / Failed: latch; producers keep running and see refusals
        break;
    }
    return accepted;
}

AudioCaptureRing::AudioCaptureRing(std::size_t frameCapacity, unsigned channels) : channels_(channels), capacity_(frameCapacity) {
    if (channels == 0 || channels > 8 || frameCapacity == 0 || frameCapacity > (std::size_t(1) << 26)) { channels_ = 0; return; }
    buf_.assign(frameCapacity * channels, 0.0f);
}

bool AudioCaptureRing::push(const float* in, std::size_t frames) noexcept {
    if (!valid() || !in) return false;
    const std::uint64_t head = head_.load(std::memory_order_relaxed), tail = tail_.load(std::memory_order_acquire);
    if (frames > capacity_ - std::size_t(head - tail)) { overflow_.store(true, std::memory_order_release); return false; }
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t at = std::size_t((head + i) % capacity_) * channels_;
        for (unsigned c = 0; c < channels_; ++c) buf_[at + c] = in[i * channels_ + c];
    }
    head_.store(head + frames, std::memory_order_release);
    return true;
}

std::size_t AudioCaptureRing::pop(float* out, std::size_t maxFrames) noexcept {
    if (!valid() || !out) return 0;
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed), head = head_.load(std::memory_order_acquire);
    const std::size_t n = std::min<std::size_t>(maxFrames, std::size_t(head - tail));
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t at = std::size_t((tail + i) % capacity_) * channels_;
        for (unsigned c = 0; c < channels_; ++c) out[i * channels_ + c] = buf_[at + c];
    }
    tail_.store(tail + n, std::memory_order_release);
    return n;
}

std::size_t AudioCaptureRing::available() const noexcept {
    return std::size_t(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
}

std::size_t captureBudgetBytes(std::uint32_t slots, std::uint32_t width, std::uint32_t height, double audioSeconds,
                               double sampleRate, unsigned channels) {
    const double video = double(slots) * width * height * 4.0;
    const double audio = audioSeconds * sampleRate * channels * sizeof(float);
    const double total = video + audio;
    return total >= 9.0e18 ? std::numeric_limits<std::size_t>::max() : std::size_t(total);
}

}  // namespace cd::media
