#include "media/Capture.h"
#include "media/PngWriter.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace cd::media {

namespace {
std::size_t ringChunks(std::uint32_t rate, double seconds) {
    const double c = std::ceil(double(rate) * seconds / double(kAudioChunkFrames));
    return std::size_t(std::max(2.0, c));
}
std::uint64_t pow2Ceil(std::uint64_t v) {
    std::uint64_t c = 1;
    while (c < v) c <<= 1;
    return c;
}
bool validRate(std::uint32_t r) { return r >= 8000 && r <= 384000; }
}  // namespace

CapturePlan planCapture(const CaptureConfig& cfg) {
    CapturePlan p;
    p.capBytes = cfg.memoryCapBytes;
    if (cfg.width < 1 || cfg.height < 1 || cfg.width > kMaxImageDim || cfg.height > kMaxImageDim ||
        cfg.cpuBuffers < 2 || cfg.cpuBuffers > 3 || !validRate(cfg.sampleRate) ||
        !(cfg.audioRingSeconds > 0.0 && cfg.audioRingSeconds <= 10.0)) {
        p.error = MediaError::InvalidArgument;
        p.reason = "invalid capture configuration (dimensions, buffers, rate or ring length)";
        return p;
    }
    const std::uint64_t px = std::uint64_t(cfg.width) * std::uint64_t(cfg.height);
    p.frameBytes = px * 4;
    p.stagingBytes = px * 5;  // held RGBA frame + GIF index buffer
    p.overheadBytes = 512u * 1024u;
    p.audioRingBytes = cfg.captureAudio ? pow2Ceil(ringChunks(cfg.sampleRate, cfg.audioRingSeconds)) * sizeof(AudioBlock) : 0;
    for (int bufs = cfg.cpuBuffers; bufs >= 2; --bufs) {
        p.cpuBuffers = bufs;
        p.totalBytes = p.frameBytes * std::uint64_t(bufs) + p.audioRingBytes + p.stagingBytes + p.overheadBytes;
        if (p.totalBytes <= cfg.memoryCapBytes) return p;
    }
    p.error = MediaError::MemoryCap;
    p.reason = "capture memory plan (" + std::to_string(p.totalBytes) + " B with 2 CPU buffers) exceeds the cap (" +
               std::to_string(cfg.memoryCapBytes) + " B); reduce the capture size (e.g. 720x1280)";
    return p;
}

// ---- AudioTap --------------------------------------------------------------------------------
AudioTap::AudioTap(std::uint32_t rate, double seconds) : ring_(ringChunks(rate, seconds)) {}

void AudioTap::push(const float* left, const float* right, std::uint32_t frames, std::int64_t start) noexcept {
    clock_.store(start + std::int64_t(frames), std::memory_order_relaxed);
    if (frames == 0 || !left || !enabled_.load(std::memory_order_acquire)) return;
    std::uint32_t done = 0;
    while (done < frames) {
        const std::uint32_t n = std::min(kAudioChunkFrames, frames - done);
        if (overflow_.load(std::memory_order_relaxed)) {
            const std::uint32_t rest = frames - done;
            dropped_.fetch_add((rest + kAudioChunkFrames - 1) / kAudioChunkFrames, std::memory_order_relaxed);
            return;
        }
        AudioBlock b;
        b.startSample = start + std::int64_t(done);
        b.frames = n;
        const float* r = right ? right : left;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = left[done + i], rr = r[done + i];
            b.samples[2 * i] = std::isfinite(l) ? l : 0.0f;
            b.samples[2 * i + 1] = std::isfinite(rr) ? rr : 0.0f;
        }
        for (std::uint32_t i = 2 * n; i < 2 * kAudioChunkFrames; ++i) b.samples[i] = 0.0f;
        if (!ring_.tryPush(b)) {
            overflow_.store(true, std::memory_order_release);
            const std::uint32_t rest = frames - done;
            dropped_.fetch_add((rest + kAudioChunkFrames - 1) / kAudioChunkFrames, std::memory_order_relaxed);
            return;
        }
        done += n;
    }
}

// ---- FramePool -------------------------------------------------------------------------------
FramePool::FramePool(int w, int h, int slots)
    : frameBytes_(std::size_t(w) * std::size_t(h) * 4), storage_(frameBytes_ * std::size_t(slots)),
      ready_(std::size_t(slots)), free_(std::size_t(slots)) {
    for (int i = 0; i < slots; ++i) free_.tryPush(std::int32_t(i));
}

std::uint8_t* FramePool::acquire(int& slotOut) noexcept {
    std::int32_t s;
    if (!free_.tryPop(s)) { dropped_.fetch_add(1, std::memory_order_relaxed); return nullptr; }
    slotOut = s;
    return storage_.data() + std::size_t(s) * frameBytes_;
}
bool FramePool::submit(int slot, std::int64_t pts) noexcept { return ready_.tryPush(FrameRef{std::int32_t(slot), pts}); }
void FramePool::release(int slot) noexcept { free_.tryPush(std::int32_t(slot)); }

// ---- CaptureSession --------------------------------------------------------------------------
MediaError CaptureSession::create(const CaptureConfig& cfg, std::unique_ptr<IVideoEncoder> enc, ByteSink* audioSink,
                                  std::unique_ptr<CaptureSession>& out, std::string& reason) {
    out.reset();
    auto bad = [&](MediaError e, const char* why) { reason = why; return e; };
    if (!enc) return bad(MediaError::Unavailable, "no encoder");
    const bool single = enc->maxFrames() == 1;
    if (!(cfg.fps >= 1 && cfg.fps <= 60)) return bad(MediaError::InvalidArgument, "fps must be 1..60");
    if (!(cfg.durationSec > 0.0) || !std::isfinite(cfg.durationSec)) return bad(MediaError::InvalidArgument, "duration must be > 0");
    if (!single && enc->maxDurationSec() > 0.0 && cfg.durationSec > enc->maxDurationSec())
        return bad(MediaError::InvalidArgument, "duration exceeds the format maximum");
    if (single && cfg.captureAudio) return bad(MediaError::InvalidArgument, "a still image has no audio");
    if (cfg.captureAudio && !audioSink) return bad(MediaError::InvalidArgument, "audio sink required");
    CaptureConfig c = cfg;
    const CapturePlan plan = planCapture(c);
    if (plan.error != MediaError::Ok) { reason = plan.reason; return plan.error; }

    std::unique_ptr<CaptureSession> s(new CaptureSession());
    s->cfg_ = c;
    s->plan_ = plan;
    s->enc_ = std::move(enc);
    s->audioSink_ = c.captureAudio ? audioSink : nullptr;
    s->singleShot_ = single;
    s->tap_ = std::make_unique<AudioTap>(c.sampleRate, c.audioRingSeconds);
    s->pool_ = std::make_unique<FramePool>(c.width, c.height, plan.cpuBuffers);
    s->held_.resize(std::size_t(c.width) * std::size_t(c.height) * 4);
    if (c.captureAudio) s->wav_ = std::make_unique<WavWriter>(*audioSink, 2, c.sampleRate, c.wavFormat);
    s->durationSamples_ = std::int64_t(std::llround(c.durationSec * double(c.sampleRate)));
    s->maxTicks_ = single ? 1 : std::max<std::int64_t>(1, std::int64_t(std::llround(c.durationSec * double(c.fps))));
    s->tol_ = std::int64_t(c.sampleRate) / (2 * c.fps);
    s->sync_ = AvSyncModel(c.sampleRate);
    out = std::move(s);
    return MediaError::Ok;
}

CaptureSession::~CaptureSession() {
    quit_.store(true);
    if (!terminal()) cancel();
    join();
    if (!terminal()) doCancel();
}

bool CaptureSession::terminal() const noexcept {
    const CaptureState s = state();
    return s == CaptureState::Completed || s == CaptureState::Cancelled || s == CaptureState::Failed;
}

MediaError CaptureSession::start() {
    int expected = int(CaptureState::Armed);
    if (!state_.compare_exchange_strong(expected, int(CaptureState::Recording))) return MediaError::BadState;
    tap_->setEnabled(cfg_.captureAudio);
    return MediaError::Ok;
}
void CaptureSession::stop() {
    tap_->setEnabled(false);
    stopReq_.store(true, std::memory_order_release);
}
void CaptureSession::cancel() {
    tap_->setEnabled(false);
    cancelReq_.store(true, std::memory_order_release);
}

void CaptureSession::fail(MediaError e) {
    if (terminal()) return;
    error_.store(int(e), std::memory_order_release);
    tap_->setEnabled(false);
    if (enc_) enc_->abort();
    if (audioSink_) audioSink_->abort();
    setState(e == MediaError::Cancelled ? CaptureState::Cancelled : CaptureState::Failed);
}
void CaptureSession::doCancel() { fail(MediaError::Cancelled); }

void CaptureSession::ensureBegun() {
    if (begun_) return;
    begun_ = true;
    MediaError e = enc_->begin();
    if (e != MediaError::Ok) { fail(e); return; }
    if (wav_) {
        e = wav_->begin();
        if (e != MediaError::Ok) fail(e);
    }
}

bool CaptureSession::pump() {
    if (cancelReq_.load(std::memory_order_acquire) && !terminal()) doCancel();
    if (terminal()) {  // keep producers unblocked and queues empty
        AudioBlock b;
        while (tap_->pop(b)) {}
        FrameRef f;
        while (pool_->pop(f)) pool_->release(f.slot);
        return false;
    }
    if (state() == CaptureState::Armed) return true;
    ensureBegun();
    if (terminal()) return false;
    drainAudio();
    if (!terminal()) drainFrames();
    if (!terminal() && (stopReq_.load(std::memory_order_acquire) || durationReached_)) finalize();
    return !terminal();
}

void CaptureSession::drainAudio() {
    if (tap_->overflowed()) { fail(MediaError::AudioOverflow); return; }
    AudioBlock b;
    while (!terminal() && tap_->pop(b)) handleAudio(b);
}

void CaptureSession::drainFrames() {
    FrameRef f;
    while (!terminal() && (haveS0_ || !cfg_.captureAudio) && pool_->pop(f)) handleFrame(f);
}

void CaptureSession::handleAudio(const AudioBlock& b) {
    if (!cfg_.captureAudio || audioEnded_) return;
    if (!haveS0_) { s0_ = b.startSample; expectedNext_ = s0_; haveS0_ = true; }
    if (b.startSample != expectedNext_) {  // seek / non-contiguous: finalize the segment, flag it
        st_.discontinuity = true;
        audioEnded_ = true;
        stopReq_.store(true, std::memory_order_release);
        return;
    }
    const std::int64_t avail = durationSamples_ - (b.startSample - s0_);
    const std::int64_t n = std::min<std::int64_t>(b.frames, avail);
    if (n <= 0) { durationReached_ = true; audioEnded_ = true; return; }
    const MediaError e = wav_->append(b.samples, std::uint32_t(n));
    if (e != MediaError::Ok) { fail(e); return; }
    expectedNext_ += std::int64_t(b.frames);
    audioEnd_ = b.startSample + n;
    st_.audioFramesWritten += n;
    if (audioEnd_ - s0_ >= durationSamples_) { durationReached_ = true; audioEnded_ = true; }
}

void CaptureSession::handleFrame(const FrameRef& f) {
    const std::uint8_t* px = pool_->data(f.slot);
    if (!cfg_.captureAudio && !haveS0_) { s0_ = f.pts + cfg_.outputTapLatencySamples; haveS0_ = true; }
    std::int64_t p = f.pts + cfg_.outputTapLatencySamples - s0_;
    if (p < -tol_) ++st_.framesDroppedEarly;
    else {
        if (p < 0) p = 0;
        processFrame(px, p);
    }
    pool_->release(f.slot);
}

void CaptureSession::processFrame(const std::uint8_t* px, std::int64_t p) {
    if (p < lastPts_) { ++st_.framesNonMonotonic; return; }  // never emit non-monotonic timestamps
    lastPts_ = p;
    if (singleShot_) {
        std::memcpy(held_.data(), px, held_.size());
        haveHeld_ = true; heldUsed_ = false; heldPts_ = p;
        return;
    }
    if (tickSample(maxTicks_, cfg_.sampleRate, cfg_.fps) <= p) { durationReached_ = true; return; }
    const bool first = !haveHeld_;
    if (haveHeld_) {
        flushBefore(p);
        if (!heldUsed_) ++st_.supersededFrames;
    }
    if (terminal()) return;
    std::memcpy(held_.data(), px, held_.size());
    heldPts_ = p; haveHeld_ = true; heldUsed_ = false;
    if (first) flushBefore(p);  // backfill leading ticks with the first frame (counted as repeats)
}

void CaptureSession::flushBefore(std::int64_t p) {
    while (!terminal() && tickValid() && tickSample(nextTick_, cfg_.sampleRate, cfg_.fps) + tol_ < p) emitHeld();
}

void CaptureSession::emitHeld() {
    const MediaError e = enc_->addFrame(held_.data());
    if (e != MediaError::Ok) { fail(e); return; }
    ++st_.ticksEmitted;
    if (heldUsed_) ++st_.repeatedFrames;
    heldUsed_ = true;
    sync_.record(tickSample(nextTick_, cfg_.sampleRate, cfg_.fps), heldPts_);
    ++nextTick_;
}

void CaptureSession::finalize() {
    setState(CaptureState::Finalizing);
    tap_->setEnabled(false);
    if (tap_->overflowed()) { fail(MediaError::AudioOverflow); return; }
    if (!haveHeld_) { fail(MediaError::NoData); return; }
    if (singleShot_) {
        emitHeld();
    } else if (cfg_.captureAudio) {
        const std::int64_t elapsed = std::min(audioEnd_ - s0_, durationSamples_);
        while (!terminal() && tickValid() && tickSample(nextTick_, cfg_.sampleRate, cfg_.fps) < elapsed) emitHeld();
    } else {
        while (!terminal() && tickValid() && tickSample(nextTick_, cfg_.sampleRate, cfg_.fps) <= lastPts_ + tol_) emitHeld();
    }
    if (!terminal() && st_.ticksEmitted == 0) emitHeld();
    if (terminal()) return;
    MediaError e = enc_->finish();
    if (e == MediaError::Ok && wav_) e = wav_->finish();
    if (e != MediaError::Ok) { fail(e); return; }
    if (!enc_->commit()) { fail(MediaError::SinkWriteFailed); return; }
    if (audioSink_ && !audioSink_->commit()) { fail(MediaError::SinkWriteFailed); return; }
    setState(CaptureState::Completed);
}

void CaptureSession::startWorker() {
    if (thread_.joinable()) return;
    thread_ = std::thread([this] {
        while (!quit_.load(std::memory_order_acquire) && pump())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
}
void CaptureSession::join() {
    if (thread_.joinable()) thread_.join();
}

CaptureStats CaptureSession::stats() const {
    CaptureStats s = st_;
    s.framesDroppedNoSlot = pool_->droppedNoSlot();
    s.audioChunksDropped = tap_->droppedChunks();
    s.maxAbsSkewMs = sync_.maxAbsSkewMs();
    s.driftMs = sync_.driftMs();
    s.plannedBytes = plan_.totalBytes;
    s.repeatedOver2Percent = s.ticksEmitted > 0 && double(s.repeatedFrames) > 0.02 * double(s.ticksEmitted);
    return s;
}

}  // namespace cd::media
