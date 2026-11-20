// cd_media tests (T03 worker-side parts). Synthetic fixtures only; MP4 is the G07 stub.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <type_traits>
#include "MediaDecoders.h"
#include "media/AvSync.h"
#include "media/CaptureQueue.h"
#include "media/GifEncoder.h"
#include "media/Mp4Encoder.h"
#include "media/PngEncoder.h"

namespace {
using namespace cd::media;
using mediatest::Bytes;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

Bytes unhex(const char* h) {
    Bytes b;
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (; h[0] && h[1]; h += 2) b.push_back(std::uint8_t(nib(h[0]) * 16 + nib(h[1])));
    return b;
}

struct Lcg { std::uint32_t s; std::uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; } };

Bytes makeImage(unsigned w, unsigned h, unsigned colours, std::uint32_t seed, bool alpha) {
    Lcg r{seed};
    std::vector<std::uint32_t> pal(colours);
    for (auto& c : pal) c = r.next() & 0xFFFFFF;
    // Make palette entries distinct.
    for (unsigned i = 0; i < colours; ++i) pal[i] = (pal[i] & 0xFFFFFC) | 0;  // keep spread
    for (unsigned i = 0; i < colours; ++i) pal[i] = ((pal[i] & 0xFFFF00) | (i & 0xFF)) ^ (std::uint32_t(i >> 8) << 16);
    Bytes px(std::size_t(w) * h * 4);
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        const auto c = pal[r.next() % colours];
        px[i * 4] = std::uint8_t(c >> 16); px[i * 4 + 1] = std::uint8_t(c >> 8); px[i * 4 + 2] = std::uint8_t(c);
        px[i * 4 + 3] = alpha ? std::uint8_t(r.next()) : 255;
    }
    return px;
}

void testChecksums() {
    const char* s = "123456789";
    CHECK(crc32(reinterpret_cast<const std::uint8_t*>(s), 9) == 0xCBF43926u);
    const char* w = "Wikipedia";
    CHECK(adler32(reinterpret_cast<const std::uint8_t*>(w), 9) == 0x11E60398u);
    Bytes big(100000, 0xFF);  // exercises the modulo chunking
    CHECK(adler32(big.data(), big.size()) == adler32(big.data() + 50000, 50000, adler32(big.data(), 50000)));
}

void testInflateFixed() {
    const Bytes z = unhex("78da4b4c4a4e444211915189448800003256145c");
    Bytes out;
    CHECK(mediatest::inflate(z.data() + 2, z.size() - 6, out));
    std::string expect; for (int i = 0; i < 3; ++i) expect += "abcabcabcabcabcXYZ";
    CHECK(std::string(out.begin(), out.end()) == expect);
}

void testPng() {
    for (auto dims : {std::pair<unsigned, unsigned>{1, 1}, {37, 23}, {300, 300}, {16384 / 4, 5}}) {
        const Bytes px = makeImage(dims.first, dims.second, 4000, 7 + dims.first, true);
        Bytes png; std::string err;
        CHECK(encodePng(px.data(), px.size(), dims.first, dims.second, png, err));
        mediatest::DecodedPng d;
        CHECK(mediatest::decodePng(png, d));
        CHECK(d.w == dims.first && d.h == dims.second);
        CHECK(d.rgba == px);
        Bytes again; CHECK(encodePng(px.data(), px.size(), dims.first, dims.second, again, err) && again == png);  // deterministic
    }
    Bytes png; std::string err;
    const Bytes px(16, 0);
    CHECK(!encodePng(px.data(), px.size(), 0, 4, png, err));
    CHECK(encodePng(px.data(), px.size(), 2, 2, png, err));              // 2x2 RGBA = 16 bytes: valid
    CHECK(!encodePng(px.data(), 15, 2, 2, png, err));                    // size mismatch
    CHECK(!encodePng(nullptr, 16, 2, 2, png, err));
    PngLimits lim; lim.maxDimension = 8;
    const Bytes bigpx(9 * 1 * 4, 0);
    CHECK(encodePng(bigpx.data(), bigpx.size(), 9, 1, png, err));       // fine under the default limits
    CHECK(!encodePng(bigpx.data(), bigpx.size(), 9, 1, png, err, lim) && err.find("limit") != std::string::npos);
    lim.maxDimension = 4096; lim.maxRawBytes = 10;
    CHECK(!encodePng(px.data(), px.size(), 2, 2, png, err, lim));        // raw size 18 > cap 10
    const Bytes huge(4097u * 4, 0);
    CHECK(!encodePng(huge.data(), huge.size(), 4097, 1, png, err));      // over default dimension cap
    // Corrupted CRC rejected by the decoder.
    const Bytes ok = makeImage(8, 8, 5, 1, false);
    CHECK(encodePng(ok.data(), ok.size(), 8, 8, png, err));
    png[40] ^= 1;
    mediatest::DecodedPng d; CHECK(!mediatest::decodePng(png, d));
}

void testGif() {
    std::string err;
    // Frame set: exact palettes, a subset palette (reuses global), a disjoint palette (local table), 2 colours.
    const unsigned w = 64, h = 48;
    std::vector<Bytes> frames = {makeImage(w, h, 200, 1, false), makeImage(w, h, 200, 1, false), makeImage(w, h, 17, 99, false), makeImage(w, h, 2, 5, false)};
    // Frame 1: a strict subset of frame 0's colours.
    for (std::size_t i = 0; i < frames[1].size(); i += 4) if ((i / 4) % 3 == 0) { frames[1][i] = frames[0][0]; frames[1][i + 1] = frames[0][1]; frames[1][i + 2] = frames[0][2]; }
    GifWriter gw;
    CHECK(gw.begin(w, h, {}, {}, err));
    for (auto& f : frames) CHECK(gw.addFrame(f.data(), f.size(), err));
    Bytes gif; CHECK(gw.finish(gif, err));
    mediatest::DecodedGif d;
    CHECK(mediatest::decodeGif(gif, d));
    CHECK(d.w == w && d.h == h && d.trailer);
    CHECK(d.hasLoopExt && d.loops == 0);
    CHECK(d.frames.size() == frames.size());
    for (std::size_t i = 0; i < d.frames.size() && i < frames.size(); ++i) {
        CHECK(d.frames[i].delayCs == 5);  // 50 ms
        CHECK(d.frames[i].rgba == frames[i]);  // pixel exact for palette-exact images
    }
    // Loop off: no extension.
    GifWriter once; GifOptions o; o.loop = false; o.delayCentiseconds = 5;
    CHECK(once.begin(w, h, o, {}, err) && once.addFrame(frames[0].data(), frames[0].size(), err));
    Bytes g2; CHECK(once.finish(g2, err));
    mediatest::DecodedGif d2; CHECK(mediatest::decodeGif(g2, d2) && !d2.hasLoopExt);
    // Determinism.
    GifWriter again; CHECK(again.begin(w, h, {}, {}, err));
    for (auto& f : frames) CHECK(again.addFrame(f.data(), f.size(), err));
    Bytes g3; CHECK(again.finish(g3, err) && g3 == gif);

    // LZW code-size reset: 256 colours, high-entropy 300x300 overflows the 4096-entry table many times.
    const Bytes noisy = makeImage(300, 300, 256, 11, false);
    GifWriter big; CHECK(big.begin(300, 300, {}, {}, err) && big.addFrame(noisy.data(), noisy.size(), err));
    Bytes g4; CHECK(big.finish(g4, err));
    mediatest::DecodedGif d4; CHECK(mediatest::decodeGif(g4, d4) && d4.frames.size() == 1 && d4.frames[0].rgba == noisy);

    // Quantised (> 256 colours): deterministic, <= 256 palette entries, right size, bounded error.
    const Bytes rich = makeImage(120, 90, 3000, 3, false);
    const auto q1 = quantizeRgba(rich.data(), 120 * 90), q2 = quantizeRgba(rich.data(), 120 * 90);
    CHECK(!q1.exact && q1.palette.size() <= 256 * 3 && q1.palette == q2.palette && q1.indices == q2.indices);
    GifWriter qw; CHECK(qw.begin(120, 90, {}, {}, err) && qw.addFrame(rich.data(), rich.size(), err));
    Bytes g5; CHECK(qw.finish(g5, err));
    mediatest::DecodedGif d5; CHECK(mediatest::decodeGif(g5, d5) && d5.frames[0].rgba.size() == rich.size());
    // Plain gradient: quantisation error stays small.
    Bytes grad(64 * 64 * 4, 255);
    for (int i = 0; i < 64 * 64; ++i) { grad[std::size_t(i) * 4] = std::uint8_t(i % 64 * 4); grad[std::size_t(i) * 4 + 1] = std::uint8_t(i / 64 * 4); grad[std::size_t(i) * 4 + 2] = std::uint8_t((i % 64 + i / 64) * 2); }
    GifWriter gg; CHECK(gg.begin(64, 64, {}, {}, err) && gg.addFrame(grad.data(), grad.size(), err));
    Bytes g6; CHECK(gg.finish(g6, err));
    mediatest::DecodedGif d6; CHECK(mediatest::decodeGif(g6, d6));
    int worst = 0;
    for (std::size_t i = 0; i < grad.size(); ++i) worst = std::max(worst, std::abs(int(grad[i]) - int(d6.frames[0].rgba[i])));
    CHECK(worst <= 40);

    // Caps and misuse.
    GifWriter c1; CHECK(!c1.begin(0, 10, {}, {}, err));
    GifLimits l; l.maxDimension = 100;
    GifWriter c2; CHECK(!c2.begin(101, 10, {}, l, err));
    l.maxFrames = 2; l.maxDimension = 16;
    GifWriter c3; CHECK(c3.begin(4, 4, {}, l, err));
    const Bytes tiny(4 * 4 * 4, 9);
    CHECK(c3.addFrame(tiny.data(), tiny.size(), err) && c3.addFrame(tiny.data(), tiny.size(), err));
    CHECK(!c3.addFrame(tiny.data(), tiny.size(), err) && err.find("cap") != std::string::npos);
    CHECK(!c3.addFrame(tiny.data(), tiny.size() - 4, err));
    GifWriter c4; CHECK(c4.begin(4, 4, {}, {}, err)); Bytes none; CHECK(!c4.finish(none, err));
    // Single-colour frame.
    GifWriter c5; CHECK(c5.begin(4, 4, {}, {}, err) && c5.addFrame(tiny.data(), tiny.size(), err)); Bytes g7; CHECK(c5.finish(g7, err));
    mediatest::DecodedGif d7; CHECK(mediatest::decodeGif(g7, d7) && d7.frames[0].rgba.size() == tiny.size() && d7.frames[0].rgba[0] == 9);
}

void testGifByteLimits() {
    static_assert(!std::is_copy_constructible_v<GifWriter> && !std::is_move_constructible_v<GifWriter>);
    std::string error;
    const Bytes pixels = makeImage(4, 4, 2, 42, false);
    GifLimits limits; limits.maxBytes = 4096;
    GifWriter reference;
    CHECK(reference.begin(4, 4, {}, limits, error));
    CHECK(reference.addFrame(pixels.data(), pixels.size(), error));
    Bytes expected;
    CHECK(reference.finish(expected, error));
    CHECK(!expected.empty());
    CHECK(expected.capacity() == limits.maxBytes);  // The reserved output is moved, not copied.
    if (expected.empty()) return;

    // Every boundary of this small file exercises header, palettes, extensions,
    // image descriptor, LZW subblocks and the trailer. Exact total size succeeds.
    const Bytes sentinel{0x13, 0x37};
    for (std::size_t cap = 1; cap <= expected.size(); ++cap) {
        limits.maxBytes = cap;
        GifWriter bounded;
        CHECK(bounded.begin(4, 4, {}, limits, error));
        const bool added = bounded.addFrame(pixels.data(), pixels.size(), error);
        CHECK(added == (cap >= expected.size() - 1));
        CHECK(bounded.bytesSoFar() <= cap);
        CHECK(bounded.frameCount() == (added ? 1u : 0u));
        Bytes out = sentinel;
        const bool finished = bounded.finish(out, error);
        CHECK(finished == (cap == expected.size()));
        CHECK(out == (finished ? expected : sentinel));
        if (finished) {
            CHECK(out.capacity() == cap);
            mediatest::DecodedGif decoded;
            CHECK(mediatest::decodeGif(out, decoded) && decoded.trailer && decoded.frames.size() == 1);
            if (decoded.frames.size() == 1) CHECK(decoded.frames[0].rgba == pixels);
        } else CHECK(!error.empty());
    }

    // A larger LZW payload must stop within its cap, including full 255-byte blocks.
    const Bytes noisy = makeImage(32, 32, 256, 77, false);
    limits.maxBytes = 8192;
    GifWriter large;
    CHECK(large.begin(32, 32, {}, limits, error) && large.addFrame(noisy.data(), noisy.size(), error));
    Bytes largeFile;
    CHECK(large.finish(largeFile, error));
    CHECK(largeFile.size() > 1024);
    if (largeFile.size() <= 1024) return;
    for (const std::size_t cap : {std::size_t(1024), largeFile.size() - 2, largeFile.size() - 1, largeFile.size()}) {
        limits.maxBytes = cap;
        GifWriter bounded;
        CHECK(bounded.begin(32, 32, {}, limits, error));
        const bool added = bounded.addFrame(noisy.data(), noisy.size(), error);
        CHECK(added == (cap >= largeFile.size() - 1));
        CHECK(bounded.bytesSoFar() <= cap);
        Bytes out = sentinel;
        const bool finished = bounded.finish(out, error);
        CHECK(finished == (cap == largeFile.size()));
        CHECK(out == (finished ? largeFile : sentinel));
    }

    // Impossible capacities fail before allocation; the writer remains reusable.
    GifWriter invalid;
    limits.maxBytes = std::numeric_limits<std::size_t>::max();
    CHECK(!invalid.begin(4, 4, {}, limits, error));
    limits.maxBytes = 0;
    CHECK(!invalid.begin(4, 4, {}, limits, error));
    limits.maxBytes = expected.size();
    CHECK(invalid.begin(4, 4, {}, limits, error) && error.empty());
    CHECK(invalid.addFrame(pixels.data(), pixels.size(), error));
    Bytes exact;
    CHECK(invalid.finish(exact, error) && exact == expected);

    constexpr auto unavailable = std::numeric_limits<std::size_t>::max();
    constexpr std::size_t outputCap = 32u * 1024u * 1024u;
    constexpr std::size_t maxPixels = 540u * 960u;
    const auto admitted = gifEncodingMemoryBound(540, 960, outputCap);
    CHECK(admitted == outputCap + maxPixels * std::max(std::size_t(12), std::size_t(10) + sizeof(std::size_t)) + 128u * 1024u);
    CHECK(admitted == gifEncodingMemoryBound(960, 540, outputCap));
    CHECK(admitted + maxPixels * 4 * 8 < 64u * 1024u * 1024u);
    CHECK(gifEncodingMemoryBound(0, 960, outputCap) == unavailable);
    CHECK(gifEncodingMemoryBound(540, 0, outputCap) == unavailable);
    CHECK(gifEncodingMemoryBound(65536, 1, outputCap) == unavailable);
    CHECK(gifEncodingMemoryBound(1, 65536, outputCap) == unavailable);
    CHECK(gifEncodingMemoryBound(1, 1, 0) == unavailable);
    CHECK(gifEncodingMemoryBound(1, 1, unavailable) == unavailable);
}

void testGifRepeatedFrames() {
    std::string error;
    constexpr unsigned width = 32, height = 24;
    const auto first = makeImage(width, height, 7, 1, false);
    const auto local = makeImage(width, height, 13, 99, false);
    const auto rich = makeImage(width, height, 1000, 17, false);
    GifLimits limits; limits.maxFrames = 8; limits.maxBytes = 64 * 1024;
    for (bool loop : {false, true}) {
        GifOptions options; options.loop = loop;
        GifWriter writer;
        CHECK(!writer.repeatLastFrame(error));
        CHECK(writer.begin(width, height, options, limits, error));
        CHECK(!writer.repeatLastFrame(error));
        Bytes producer = first;
        CHECK(writer.addFrame(producer.data(), producer.size(), error));
        // Repeating depends only on encoded bytes, even after producer storage changes.
        producer.assign(producer.size(), 0);
        CHECK(writer.repeatLastFrame(error) && writer.repeatLastFrame(error));
        CHECK(writer.addFrame(local.data(), local.size(), error));
        CHECK(writer.repeatLastFrame(error) && writer.repeatLastFrame(error));
        CHECK(writer.addFrame(rich.data(), rich.size(), error) && writer.repeatLastFrame(error));
        CHECK(writer.frameCount() == limits.maxFrames);
        CHECK(!writer.repeatLastFrame(error) && error.find("cap") != std::string::npos);
        Bytes gif;
        CHECK(writer.finish(gif, error));
        CHECK(!writer.repeatLastFrame(error));
        mediatest::DecodedGif decoded;
        CHECK(mediatest::decodeGif(gif, decoded));
        CHECK(decoded.trailer && decoded.hasLoopExt == loop && decoded.frames.size() == 8);
        if (decoded.frames.size() == 8) {
            for (const auto& frame : decoded.frames) CHECK(frame.delayCs == 5);
            for (std::size_t i = 0; i < 3; ++i) CHECK(decoded.frames[i].rgba == first);
            for (std::size_t i = 3; i < 6; ++i) CHECK(decoded.frames[i].rgba == local);
            CHECK(decoded.frames[6].rgba == decoded.frames[7].rgba);
        }
    }

    GifWriter reference;
    CHECK(reference.begin(width, height, {}, limits, error));
    CHECK(reference.addFrame(first.data(), first.size(), error) && reference.repeatLastFrame(error));
    Bytes expected;
    CHECK(reference.finish(expected, error));
    CHECK(expected.size() > 2);
    if (expected.size() <= 2) return;
    for (std::size_t cap = expected.size() - 2; cap <= expected.size(); ++cap) {
        limits.maxBytes = cap;
        GifWriter writer;
        CHECK(writer.begin(width, height, {}, limits, error));
        CHECK(writer.addFrame(first.data(), first.size(), error));
        const auto before = writer.bytesSoFar();
        const bool repeated = writer.repeatLastFrame(error);
        CHECK(repeated == (cap >= expected.size() - 1));
        CHECK(writer.bytesSoFar() <= cap);
        if (!repeated) CHECK(writer.bytesSoFar() == before && writer.frameCount() == 1);
        const Bytes sentinel{0x42};
        Bytes out = sentinel;
        const bool finished = writer.finish(out, error);
        CHECK(finished == (cap == expected.size()));
        CHECK(out == (finished ? expected : sentinel));
    }
}

void testQueue() {
    CaptureQueueConfig cfg; cfg.slotCount = 3; cfg.slotBytes = 64; cfg.memoryCapBytes = 1000;
    CaptureQueue q(cfg); CHECK(q.valid());
    CaptureQueueConfig bad = cfg; bad.memoryCapBytes = 100;
    CHECK(!CaptureQueue(bad).valid());
    bad = cfg; bad.slotCount = 0; CHECK(!CaptureQueue(bad).valid());
    bad = cfg; bad.slotBytes = std::size_t(-1) / 2; CHECK(!CaptureQueue(bad).valid());
    Bytes f(64, 0);
    for (int i = 0; i < 5; ++i) { f.assign(64, std::uint8_t(i)); CHECK(q.push(f.data(), 64, 8, 2, i * 100)); }
    CHECK(q.stats().droppedOldest == 2);          // 5 pushes into 3 slots
    CaptureFrame fr; int expect = 2;
    while (q.acquire(fr)) { CHECK(fr.data[0] == expect && fr.ptsSamples == expect * 100); ++expect; q.release(fr); }
    CHECK(expect == 5);
    CHECK(!q.push(f.data(), 65, 8, 2, 0) && q.stats().refusedOversize == 1);

    cfg.policy = OverflowPolicy::Refuse;
    CaptureQueue r(cfg);
    for (int i = 0; i < 5; ++i) { f.assign(64, std::uint8_t(i)); r.push(f.data(), 64, 8, 2, i); }
    CHECK(r.stats().pushed == 3 && r.stats().refusedFull == 2);
    CHECK(r.acquire(fr) && fr.data[0] == 0);       // oldest kept
    CHECK(!r.push(f.data(), 64, 8, 2, 9));          // Refuse: the slot being read is not reusable until release
    r.release(fr);

    // Failing sink latches Failed; producers get immediate refusals.
    struct Sink : FrameSink { SinkStatus st; SinkStatus consume(const CaptureFrame&) override { return st; } } sink;
    CaptureQueue s(cfg);
    for (int i = 0; i < 3; ++i) s.push(f.data(), 64, 8, 2, i);
    sink.st = SinkStatus::Stalled;
    CHECK(drainQueue(s, sink, 10) == 0 && s.stats().sinkStalls == 3 && !s.failed());
    s.push(f.data(), 64, 8, 2, 5);
    sink.st = SinkStatus::DiskFull;
    CHECK(drainQueue(s, sink, 10) == 0 && s.failed());
    CHECK(!s.push(f.data(), 64, 8, 2, 6) && s.stats().refusedFailed == 1);

    // Threaded: slow consumer never stalls the producer; frames are never torn; order is monotonic.
    CaptureQueueConfig tc; tc.slotCount = 3; tc.slotBytes = 4096; tc.policy = OverflowPolicy::DropOldest;
    CaptureQueue tq(tc);
    std::atomic<bool> done{false}; std::atomic<int> torn{0}; std::int64_t lastPts = -1; std::atomic<int> disorder{0}; std::atomic<int> got{0};
    std::thread consumer([&] {
        CaptureFrame c;
        while (!done.load() || tq.stats().consumed < tq.stats().pushed - tq.stats().droppedOldest) {
            if (tq.acquire(c)) {
                for (std::size_t i = 1; i < c.bytes; ++i) if (c.data[i] != c.data[0]) { torn++; break; }
                if (c.ptsSamples <= lastPts) disorder++;
                lastPts = c.ptsSamples; got++;
                tq.release(c);
                std::this_thread::sleep_for(std::chrono::microseconds(200));  // stalled encoder
            }
        }
    });
    Bytes frame(4096);
    // Time only push() itself (not frame preparation), so build type/instrumentation speed does
    // not masquerade as producer blocking.
    std::chrono::steady_clock::duration inPush{};
    for (int i = 0; i < 20000; ++i) {
        std::fill(frame.begin(), frame.end(), std::uint8_t(i));
        const auto t0 = std::chrono::steady_clock::now();
        tq.push(frame.data(), frame.size(), 64, 16, i);
        inPush += std::chrono::steady_clock::now() - t0;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(inPush).count();
    done = true; consumer.join();
    CHECK(torn == 0 && disorder == 0 && got > 0);
    CHECK(tq.stats().pushed + tq.stats().refusedFull == 20000);
    CHECK(ms < 5000);  // producer was never gated by the 200 us/frame consumer (would be 4 s+ if blocking)
    std::printf("  queue: producer 20000 pushes in %lld ms, consumed %d, torn %d, disorder %d, dropped %llu\n",
                static_cast<long long>(ms), got.load(), torn.load(), disorder.load(),
                static_cast<unsigned long long>(tq.stats().droppedOldest));

    // Audio ring.
    AudioCaptureRing ring(8, 2);
    float in[12], out[16];
    for (int i = 0; i < 12; ++i) in[i] = float(i);
    CHECK(ring.push(in, 6) && ring.available() == 6);
    CHECK(!ring.push(in, 3) && ring.overflowed());           // all-or-nothing, flag latched
    CHECK(ring.pop(out, 16) == 6 && out[11] == 11.0f);
    CHECK(ring.push(in, 6) && ring.push(in, 2) && ring.pop(out, 3) == 3 && ring.push(in, 3));  // wraps
    CHECK(!AudioCaptureRing(0, 2).valid());
    // Budget: 3 portrait RGBA slots + 2 s stereo at 48 kHz.
    const auto budget = captureBudgetBytes(3, 1080, 1920, 2.0, 48000, 2);
    CHECK(budget == std::size_t(3) * 1080 * 1920 * 4 + std::size_t(2 * 48000 * 2 * 4));
    CHECK(budget < 64u * 1024u * 1024u);
}

void testMp4() {
    auto enc = createPlatformMp4Encoder();
    Mp4Config cfg; cfg.destinationPath = "x.mp4";
    const auto cap = enc->probe(cfg);
    CHECK(!cap.available && cap.reason == "MP4 export unavailable on this platform (G07)");
    std::string err;
    CHECK(!enc->begin(cfg, err) && err == kMp4UnavailableReason && enc->state() == Mp4State::Failed);
    CHECK(!enc->finish(err));
    CHECK(validateMp4Config(cfg, 10, err));
    CHECK(!validateMp4Config(cfg, 61, err));
    Mp4Config odd = cfg; odd.width = 1081; CHECK(!validateMp4Config(odd, 10, err));
    odd = cfg; odd.fps = 25; CHECK(!validateMp4Config(odd, 10, err));
    odd = cfg; odd.destinationPath.clear(); CHECK(!validateMp4Config(odd, 10, err));
}

void testAv() {
    for (double fs : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0}) {
        std::vector<std::int64_t> pts;
        for (std::uint64_t i = 0; i < 60 * 30; ++i) pts.push_back(videoFramePtsSamples(i, fs));
        const auto r = analyzeAvTimestamps(pts, fs);
        CHECK(r.monotonic && r.frames == 1800 && r.maxAbsSkewMs < 0.05 && std::abs(r.driftMs) < 0.05 && r.withinBudget());
        // 150 ppm clock mismatch over 60 s = 9 ms drift: inside the 20 ms budget; 500 ppm = 30 ms: outside.
        for (auto ppm : {150.0, 500.0}) {
            std::vector<std::int64_t> skewed;
            for (auto p : pts) skewed.push_back(std::int64_t(double(p) * (1.0 + ppm * 1e-6)));
            const auto s = analyzeAvTimestamps(skewed, fs);
            CHECK(s.withinBudget() == (ppm < 400.0));
        }
        // Declared output latency is compensated.
        std::vector<std::int64_t> lat;
        for (auto p : pts) lat.push_back(p + 480);
        CHECK(analyzeAvTimestamps(lat, fs, 30, 480).maxAbsSkewMs < 0.05);
        CHECK(analyzeAvTimestamps(lat, fs, 30, 0).maxAbsSkewMs > 400.0 * 1000.0 / fs);  // uncompensated 480 samples show as skew
    }
    std::vector<std::int64_t> nm = {0, 1600, 1600, 4800};
    CHECK(!analyzeAvTimestamps(nm, 48000).monotonic);
    CHECK(samplesToMicroseconds(48000, 48000) == 1000000);

    // Encoder behind: capture only every other frame -> 50% repeats, warn.
    std::vector<std::int64_t> cap; RepeatStats rs;
    for (std::uint64_t i = 0; i < 90; i += 2) cap.push_back(videoFramePtsSamples(i, 48000));
    const auto paced = paceFrames(cap, 48000, 90);
    CHECK(paced.size() == 90);
    for (auto& p : paced) rs.frame(p.repeated);
    CHECK(rs.repeated() == 45 && rs.warn());
    for (std::size_t i = 1; i < paced.size(); ++i) CHECK(paced[i].ptsSamples > paced[i - 1].ptsSamples);  // output PTS stays monotonic
    RepeatStats ok; for (int i = 0; i < 100; ++i) ok.frame(i == 3); CHECK(!ok.warn());
    RepeatStats edge; for (int i = 0; i < 100; ++i) edge.frame(i < 3); CHECK(edge.warn());

    CHECK(exportTransitionAllowed(ExportState::Idle, ExportState::Armed));
    CHECK(!exportTransitionAllowed(ExportState::Idle, ExportState::Recording));
    CHECK(exportTransitionAllowed(ExportState::Recording, ExportState::Finalizing));
    CHECK(!exportTransitionAllowed(ExportState::Completed, ExportState::Recording));
    CHECK(exportTransitionAllowed(ExportState::Failed, ExportState::Idle));
    CHECK(exportSize(ExportFormat::Mp4, Framing::Portrait9x16).height == 1920);
    CHECK(exportSize(ExportFormat::Gif, Framing::Landscape16x9).width == 960);
    CHECK(maxDurationSeconds(ExportFormat::Gif) == 15.0 && exportFps(ExportFormat::Gif) == 20);
}
}  // namespace

int main() {
    testChecksums(); testInflateFixed(); testPng(); testGif(); testGifByteLimits(); testGifRepeatedFrames(); testQueue(); testMp4(); testAv();
    if (failures) { std::printf("%d media check(s) FAILED\n", failures); return 1; }
    std::printf("media tests passed\n");
    return 0;
}
