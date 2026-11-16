// CircuitDrift media: capture pipeline (P13, T03 partial: PNG/GIF/WAV; MP4 gate G07 OPEN).
// Owner: cd_media. Dependencies: cd_state (SpscQueue) only; no UI/JUCE/GPU code.
//
// Threads (each queue is strictly single-producer/single-consumer):
//   audio thread : AudioTap::push()  noexcept, no alloc/locks/I/O, drops (and counts) when the ring
//                  is full; it never waits on the encoder, the disk or the worker. A dropped audio
//                  chunk sets the overflow flag; the session then stops with AudioOverflow (spec:
//                  never silently discard audio, never stall processing).
//   UI/render    : FramePool::acquire()/submit() hand RGBA frames (exact selected dimensions) to
//                  the worker through a bounded pool (default 3 CPU buffers). No free buffer -> the
//                  frame is dropped and counted; the UI never blocks.
//   worker       : CaptureSession::pump() (or startWorker()) drains both queues, applies the
//                  fixed-rate timeline (repeat last frame when the UI is late, count repeats), feeds
//                  the encoder + WAV writer, and commits (temp+rename) only after full success.
// Timestamps are audio-sample-clock indices (int64). outputTapLatencySamples is a signed offset
// added to frame timestamps to compensate output-tap vs telemetry latency.
// Memory: planCapture() accounts frame pool + audio ring + staging + fixed overhead against a cap
// (default 64 MiB, the D19/D20 capture exception), reducing 3 -> 2 CPU buffers before failing.
// Failure (sink error, audio overflow, cancel) aborts every sink: no partial output remains.
// Two-file commit (video then companion WAV) is not atomic as a pair; MP4 muxing (G07) would fix it.
#pragma once
#include "media/AvSync.h"
#include "media/ByteSink.h"
#include "media/Encoder.h"
#include "media/MediaTypes.h"
#include "media/WavWriter.h"
#include "state/RtQueue.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cd::media {

struct CaptureConfig {
    int width = 540, height = 960;
    int fps = 20;                        // GIF 20 (50 ms/frame); video timelines 30
    std::uint32_t sampleRate = 48000;
    bool captureAudio = false;           // companion WAV track (stereo)
    double durationSec = 10.0;
    int cpuBuffers = 3;                  // 3 requested, reduced to 2 if the cap demands
    std::uint64_t memoryCapBytes = 64ull << 20;
    double audioRingSeconds = 2.0;
    std::int64_t outputTapLatencySamples = 0;
    WavSampleFormat wavFormat = WavSampleFormat::Float32;
};

struct CapturePlan {
    MediaError error = MediaError::Ok;
    std::string reason;
    int cpuBuffers = 0;
    std::uint64_t frameBytes = 0, audioRingBytes = 0, stagingBytes = 0, overheadBytes = 0, totalBytes = 0, capBytes = 0;
};

CapturePlan planCapture(const CaptureConfig& cfg);

inline constexpr std::uint32_t kAudioChunkFrames = 256;
struct AudioBlock {
    std::int64_t startSample;
    std::uint32_t frames;
    float samples[2 * kAudioChunkFrames];  // interleaved L,R
};

class AudioTap {
public:
    AudioTap(std::uint32_t sampleRate, double ringSeconds);
    std::size_t ringBytes() const noexcept { return ring_.capacity() * sizeof(AudioBlock); }
    void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_release); }
    // AUDIO THREAD. right may be null (mono: left is duplicated). Non-finite samples become 0.
    void push(const float* left, const float* right, std::uint32_t frames, std::int64_t startSample) noexcept;
    // Sample-clock position published by the audio side for UI-side ClockMapper observations.
    std::int64_t clockSamples() const noexcept { return clock_.load(std::memory_order_relaxed); }
    std::int64_t droppedChunks() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    bool overflowed() const noexcept { return overflow_.load(std::memory_order_acquire); }
    // WORKER THREAD.
    bool pop(AudioBlock& out) noexcept { return ring_.tryPop(out); }
private:
    cd::state::SpscQueue<AudioBlock> ring_;
    std::atomic<bool> enabled_{false}, overflow_{false};
    std::atomic<std::int64_t> dropped_{0}, clock_{0};
};

struct FrameRef { std::int32_t slot; std::int64_t pts; };

class FramePool {
public:
    FramePool(int width, int height, int slots);
    std::size_t bytes() const noexcept { return storage_.size(); }
    // UI THREAD. Returns a writable RGBA buffer (width*height*4) or null if none is free (counted).
    std::uint8_t* acquire(int& slotOut) noexcept;
    bool submit(int slot, std::int64_t ptsSamples) noexcept;
    // WORKER THREAD.
    bool pop(FrameRef& out) noexcept { return ready_.tryPop(out); }
    const std::uint8_t* data(int slot) const noexcept { return storage_.data() + std::size_t(slot) * frameBytes_; }
    void release(int slot) noexcept;
    std::int64_t droppedNoSlot() const noexcept { return dropped_.load(std::memory_order_relaxed); }
private:
    std::size_t frameBytes_;
    std::vector<std::uint8_t> storage_;
    cd::state::SpscQueue<FrameRef> ready_;
    cd::state::SpscQueue<std::int32_t> free_;
    std::atomic<std::int64_t> dropped_{0};
};

struct CaptureStats {
    std::int64_t ticksEmitted = 0, repeatedFrames = 0, supersededFrames = 0;
    std::int64_t framesDroppedEarly = 0, framesNonMonotonic = 0, framesDroppedNoSlot = 0;
    std::int64_t audioChunksDropped = 0, audioFramesWritten = 0;
    bool discontinuity = false;
    bool repeatedOver2Percent = false;   // warn threshold from the spec
    double maxAbsSkewMs = 0, driftMs = 0;
    std::uint64_t plannedBytes = 0;
};

class CaptureSession {
public:
    // Validates config, applies the memory plan, allocates everything up front. audioSink is
    // required iff cfg.captureAudio. The session starts Armed.
    static MediaError create(const CaptureConfig& cfg, std::unique_ptr<IVideoEncoder> enc, ByteSink* audioSink,
                             std::unique_ptr<CaptureSession>& out, std::string& reason);
    ~CaptureSession();
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    // Control thread.
    MediaError start();   // Armed -> Recording; enables the audio tap
    void stop();          // request finalize (drains queues, writes, commits)
    void cancel();        // request cancel (sinks aborted by the worker)
    AudioTap& audioTap() noexcept { return *tap_; }
    FramePool& frames() noexcept { return *pool_; }
    const CapturePlan& plan() const noexcept { return plan_; }
    CaptureState state() const noexcept { return CaptureState(state_.load(std::memory_order_acquire)); }
    MediaError error() const noexcept { return MediaError(error_.load(std::memory_order_acquire)); }

    // Worker. Returns true while the session is not terminal.
    bool pump();
    void startWorker();
    void join();
    // Valid on the worker thread or once the session is terminal / joined.
    CaptureStats stats() const;

private:
    CaptureSession() = default;
    void setState(CaptureState s) noexcept { state_.store(int(s), std::memory_order_release); }
    bool terminal() const noexcept;
    void fail(MediaError e);
    void doCancel();
    void ensureBegun();
    void drainAudio();
    void drainFrames();
    void handleAudio(const AudioBlock& b);
    void handleFrame(const FrameRef& f);
    void processFrame(const std::uint8_t* px, std::int64_t p);
    void emitHeld();
    void flushBefore(std::int64_t p);
    void finalize();
    bool tickValid() const noexcept { return nextTick_ < maxTicks_; }

    CaptureConfig cfg_;
    CapturePlan plan_;
    std::unique_ptr<IVideoEncoder> enc_;
    ByteSink* audioSink_ = nullptr;
    std::unique_ptr<WavWriter> wav_;
    std::unique_ptr<AudioTap> tap_;
    std::unique_ptr<FramePool> pool_;
    std::atomic<int> state_{int(CaptureState::Armed)};
    std::atomic<int> error_{int(MediaError::Ok)};
    std::atomic<bool> stopReq_{false}, cancelReq_{false}, quit_{false};
    std::thread thread_;

    // worker-only state
    bool begun_ = false, singleShot_ = false, haveS0_ = false, heldUsed_ = true, haveHeld_ = false;
    bool audioEnded_ = false, durationReached_ = false;
    std::int64_t s0_ = 0, expectedNext_ = 0, audioEnd_ = 0, durationSamples_ = 0;
    std::int64_t nextTick_ = 0, maxTicks_ = 0, lastPts_ = -1, heldPts_ = 0, tol_ = 0;
    std::vector<std::uint8_t> held_;
    AvSyncModel sync_;
    CaptureStats st_;
};

}  // namespace cd::media
