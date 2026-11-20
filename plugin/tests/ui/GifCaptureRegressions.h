// Production-session regressions. Fake time and explicit barriers drive races;
// wall-clock waits only bound hangs and do not determine capture timing.
#pragma once
#include "PluginProcessor.h"
#include "ui/GifCaptureSession.h"
#include "ui/ProcessorEditorBridge.h"
#include "../../../tests/media/MediaDecoders.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace cd::plugin::test::gif_capture_regressions {
struct FakeClock {
    std::atomic<std::int64_t> milliseconds{0};
    GifCaptureSession::Clock reader() {
        return [this] { return GifCaptureSession::TimePoint{} +
            std::chrono::milliseconds(milliseconds.load(std::memory_order_acquire)); };
    }
    void set(std::int64_t value) { milliseconds.store(value, std::memory_order_release); }
};
template<class Predicate> bool await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    void arriveAndWait() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait_for(lock, std::chrono::seconds(10), [&] { return released; });
    }
    bool awaitEntry() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        cv.notify_all();
    }
};
inline bool terminal(CaptureState state) {
    return state == CaptureState::Completed || state == CaptureState::Failed || state == CaptureState::Cancelled;
}
inline bool awaitTerminal(GifCaptureSession& session) {
    return await([&] { return terminal(session.captureState()) && session.workerFinished(); });
}
inline juce::Image frame(CaptureFraming framing, juce::Colour colour) {
    const auto size = captureDimensions(CaptureFormat::Gif, framing);
    juce::Image image(juce::Image::ARGB, size.width, size.height, true, juce::SoftwareImageType{});
    image.clear(image.getBounds(), colour);
    return image;
}
inline bool decode(const juce::File& file, mediatest::DecodedGif& decoded) {
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes) || bytes.getSize() == 0) return false;
    const auto* begin = static_cast<const std::uint8_t*>(bytes.getData());
    return mediatest::decodeGif(mediatest::Bytes(begin, begin + bytes.getSize()), decoded);
}
inline bool solid(const mediatest::GifFrameOut& image, juce::Colour colour) {
    if (image.rgba.empty() || image.rgba.size() % 4 != 0) return false;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4)
        if (image.rgba[i] != colour.getRed() || image.rgba[i + 1] != colour.getGreen() ||
            image.rgba[i + 2] != colour.getBlue() || image.rgba[i + 3] != 255) return false;
    return true;
}
inline int run() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::fprintf(stderr, "FAIL GIF capture: %s\n", message); }
    };
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("CircuitDrift-gif-" + juce::Uuid().toString());
    if (!directory.createDirectory().wasOk()) {
        check(false, "create isolated capture directory");
        return failures;
    }
    const auto requestFor = [&](const juce::String& name, CaptureFraming framing = CaptureFraming::Square1x1) {
        CaptureRequest request;
        request.seconds = 1.0;
        request.framing = framing;
        request.destination = directory.getChildFile(name);
        return request;
    };
    const auto red = juce::Colour::fromRGB(211, 23, 41);
    const auto blue = juce::Colour::fromRGB(17, 49, 223);
    const auto redSquare = frame(CaptureFraming::Square1x1, red);
    const auto blueSquare = frame(CaptureFraming::Square1x1, blue);

    for (const auto framing : {CaptureFraming::Portrait9x16, CaptureFraming::Square1x1, CaptureFraming::Landscape16x9}) {
        const auto dimensions = captureDimensions(CaptureFormat::Gif, framing);
        check((framing == CaptureFraming::Portrait9x16 && dimensions.width == 540 && dimensions.height == 960) ||
              (framing == CaptureFraming::Square1x1 && dimensions.width == 540 && dimensions.height == 540) ||
              (framing == CaptureFraming::Landscape16x9 && dimensions.width == 960 && dimensions.height == 540),
              "shared GIF dimensions retain all three declared canvases");
        check(GifCaptureSession::bufferBudgetBytes(framing) <= GifCaptureSession::kBufferLimitBytes,
              "encoding, queue, conversion, worker and render allowances fit explicit 64 MiB admission");
        FakeClock clock;
        auto request = requestFor("dimensions-" + juce::String(static_cast<int>(framing)) + ".gif", framing);
        request.loop = false;
        GifCaptureSession session(request, 17, clock.reader());
        const auto token = session.takeFrameRequest();
        check(session.ready() && token && token->width == dimensions.width && token->height == dimensions.height,
              "session admits exact declared dimensions");
        if (token) session.submit(*token, frame(framing, red));
        session.stop();
        check(awaitTerminal(session) && session.captureState() == CaptureState::Completed, "single frame manual stop completes");
        mediatest::DecodedGif gif;
        check(decode(request.destination, gif) && gif.w == static_cast<unsigned>(dimensions.width) &&
              gif.h == static_cast<unsigned>(dimensions.height) && gif.frames.size() == 1 &&
              !gif.hasLoopExt && solid(gif.frames.front(), red), "dimensions, pixels and play-once flag round trip");
    }
    check(captureDimensions(CaptureFormat::Mp4, CaptureFraming::Square1x1).width == 0 &&
          captureDimensions(CaptureFormat::Gif, static_cast<CaptureFraming>(99)).height == 0 &&
          GifCaptureSession::bufferBudgetBytes(static_cast<CaptureFraming>(99)) == std::numeric_limits<std::size_t>::max(),
          "unsupported dimensions fail admission");

    // Freeze an empty dequeue, then publish the FIRST frame and stop before the
    // worker resumes. Loading stop after that old empty observation lost it.
    {
        FakeClock clock;
        Gate empty;
        std::atomic<bool> armed{true};
        GifCaptureSession::TestHooks hooks;
        hooks.afterEmptyAcquire = [&] { if (armed.exchange(false)) empty.arriveAndWait(); };
        auto request = requestFor("first-frame-stop.gif");
        const auto destination = request.destination;
        GifCaptureSession session(request, 21, clock.reader(), std::move(hooks));
        check(empty.awaitEntry(), "worker reaches first empty dequeue barrier");
        const auto token = session.takeFrameRequest();
        request.destination = directory.getChildFile("mutated-request.gif");
        request.framing = CaptureFraming::Landscape16x9;
        request.seconds = 15;
        request.loop = false;
        if (token) session.submit(*token, redSquare);
        session.stop();
        empty.release();
        check(awaitTerminal(session) && session.captureState() == CaptureState::Completed,
              "stop cannot miss first publication after an empty dequeue");
        mediatest::DecodedGif gif;
        check(decode(destination, gif) && gif.frames.size() == 1 && gif.hasLoopExt && solid(gif.frames.front(), red) &&
              !request.destination.exists(), "capture request stays immutable after construction");
    }
    // Repeat that interleaving after encoding a frame. The final distinct visual
    // must survive stop, and a missing capture tick must retain its 50-ms slot.
    {
        FakeClock clock;
        Gate empty;
        std::atomic<bool> armed{false};
        GifCaptureSession::TestHooks hooks;
        hooks.afterEmptyAcquire = [&] { if (armed.exchange(false)) empty.arriveAndWait(); };
        const auto request = requestFor("final-frame-stop.gif");
        GifCaptureSession session(request, 22, clock.reader(), std::move(hooks));
        const auto first = session.takeFrameRequest();
        if (first) session.submit(*first, redSquare);
        check(await([&] { return session.status().framesWritten == 1; }), "first distinct frame encoded before final-frame race");
        armed.store(true);
        session.notifyClockAdvanced();
        check(empty.awaitEntry(), "worker reaches final empty dequeue barrier");
        clock.set(100);
        const auto last = session.takeFrameRequest();
        check(last && last->index == 2, "100 ms selects absolute frame two");
        if (last) session.submit(*last, blueSquare);
        session.stop();
        empty.release();
        check(awaitTerminal(session) && session.captureState() == CaptureState::Completed,
              "manual stop drains final publication before completing");
        mediatest::DecodedGif gif;
        check(decode(request.destination, gif) && gif.frames.size() == 3 &&
              solid(gif.frames.front(), red) && solid(gif.frames.back(), blue) && session.status().framesRepeated == 1,
              "manual stop preserves distinct final frame and repeats the missing tick only");
    }
    // Irregular UI opportunities use floor(elapsed / 50 ms), without resetting
    // a producer-side threshold and losing the fractional timer remainder.
    {
        FakeClock clock;
        const auto request = requestFor("jittered-timebase.gif");
        GifCaptureSession session(request, 23, clock.reader());
        const std::pair<std::int64_t, int> opportunities[] = {
            {0, 0}, {33, -1}, {66, 1}, {99, -1}, {100, 2}, {149, -1}, {150, 3}, {799, 15}, {999, 19}
        };
        for (const auto [milliseconds, expected] : opportunities) {
            clock.set(milliseconds);
            const auto token = session.takeFrameRequest();
            check(expected < 0 ? !token : token && token->index == static_cast<unsigned>(expected),
                  "each jittered opportunity returns at most one correct absolute index");
            if (token) session.submit(*token, expected == 19 ? blueSquare : redSquare);
        }
        clock.set(1000);
        session.notifyClockAdvanced(); // No further producer polls or manual stop.
        check(awaitTerminal(session) && session.captureState() == CaptureState::Completed,
              "worker independently ends recording at its deadline");
        mediatest::DecodedGif gif;
        check(decode(request.destination, gif) && gif.frames.size() == 20 && solid(gif.frames.back(), blue) &&
              std::all_of(gif.frames.begin(), gif.frames.end(), [](const auto& image) { return image.delayCs == 5; }),
              "missed UI ticks retain one second at 20 Hz and the final visual");
    }
    {
        FakeClock clock;
        auto request = requestFor("short-manual-stop.gif");
        request.seconds = 15;
        GifCaptureSession session(request, 28, clock.reader());
        session.submit(redSquare);
        session.submit(blueSquare); // Same tick: the compatibility path cannot invent a frame.
        clock.set(125);
        session.stop();
        check(awaitTerminal(session) && session.captureState() == CaptureState::Completed,
              "manual stop completes a shorter requested-duration capture");
        mediatest::DecodedGif gif;
        check(decode(request.destination, gif) && gif.frames.size() == 3 && solid(gif.frames.back(), red) &&
              session.status().framesRepeated == 2, "125 ms stop rounds up to three slots and rapid legacy submit stays single-tick");
    }
    {
        FakeClock clock;
        auto request = requestFor("maximum-duration.gif");
        request.seconds = 15;
        GifCaptureSession session(request, 29, clock.reader());
        clock.set(14999);
        const auto last = session.takeFrameRequest();
        check(last && last->index == 299, "last admitted tick is 299 at maximum duration");
        if (last) session.submit(*last, redSquare);
        clock.set(15000);
        session.notifyClockAdvanced();
        check(awaitTerminal(session) && session.captureState() == CaptureState::Completed &&
              session.status().framesWritten == 300 && session.status().framesRepeated == 299 &&
              !session.takeFrameRequest(), "first late visual fills timeline without exceeding 300-frame cap");
    }
    // Deadline while publication is admitted. The worker must wait for that
    // producer's final gate release, then drain the published data.
    {
        FakeClock clock;
        Gate publication;
        GifCaptureSession::TestHooks hooks;
        hooks.beforeFramePublication = [&] { publication.arriveAndWait(); };
        const auto request = requestFor("inflight-deadline.gif");
        GifCaptureSession session(request, 24, clock.reader(), std::move(hooks));
        const auto token = session.takeFrameRequest();
        std::thread producer([&] { if (token) session.submit(*token, redSquare); });
        check(publication.awaitEntry(), "producer reaches pre-publication barrier");
        clock.set(1000);
        session.notifyClockAdvanced();
        check(await([&] { return session.captureState() == CaptureState::Finalizing; }) && !request.destination.exists(),
              "deadline closes admission but cannot finish before active publication");
        publication.release();
        producer.join();
        check(awaitTerminal(session) && session.captureState() == CaptureState::Completed,
              "deadline drains the producer admitted before its gate closed");
        mediatest::DecodedGif gif;
        check(decode(request.destination, gif) && gif.frames.size() == 20 && solid(gif.frames.back(), red),
              "in-flight deadline preserves visual and fills the remaining requested timeline");
    }
    // An issued token alone cannot keep recording alive when the editor stops.
    {
        FakeClock clock;
        const auto request = requestFor("idle-deadline.gif");
        check(request.destination.replaceWithText("existing GIF sentinel"), "create deadline preservation fixture");
        GifCaptureSession session(request, 25, clock.reader());
        const auto abandoned = session.takeFrameRequest();
        clock.set(1000);
        session.notifyClockAdvanced();
        check(awaitTerminal(session) && session.captureState() == CaptureState::Failed &&
              session.status().message.contains("No editor frames"), "abandoned token reaches explicit deadline failure");
        if (abandoned) session.submit(*abandoned, redSquare);
        check(request.destination.loadFileAsString() == "existing GIF sentinel" && session.captureState() == CaptureState::Failed,
              "late submission cannot overwrite failed capture or existing destination");
    }

    // Allocation failure containment before and after atomic installation.
    for (const bool afterInstall : {false, true}) {
        FakeClock clock;
        GifCaptureSession::TestHooks hooks;
        if (afterInstall) hooks.beforeCompletionMessage = [] { throw std::bad_alloc{}; };
        else hooks.beforeFrameEncoding = [] { throw std::bad_alloc{}; };
        const auto request = requestFor(afterInstall ? "completion-allocation.gif" : "worker-allocation.gif");
        check(request.destination.replaceWithText("existing allocation sentinel"), "create allocation preservation fixture");
        GifCaptureSession session(request, 26, clock.reader(), std::move(hooks));
        const auto token = session.takeFrameRequest();
        if (token) session.submit(*token, redSquare);
        session.stop();
        check(awaitTerminal(session), "injected allocation failure reaches terminal state");
        if (afterInstall) {
            mediatest::DecodedGif gif;
            check(session.captureState() == CaptureState::Completed && !session.status().message.isEmpty() &&
                  decode(request.destination, gif) && gif.frames.size() == 1,
                  "success-message allocation failure cannot relabel committed valid GIF");
        } else {
            check(session.captureState() == CaptureState::Failed && !session.status().message.isEmpty() &&
                  request.destination.loadFileAsString() == "existing allocation sentinel",
                  "noexcept worker contains bad_alloc with a static reason and preserved destination");
        }
    }
    {
        FakeClock clock;
        const auto request = requestFor("invalid-frame.gif");
        check(request.destination.replaceWithText("existing invalid sentinel"), "create invalid-frame preservation fixture");
        GifCaptureSession session(request, 27, clock.reader());
        const auto token = session.takeFrameRequest();
        if (token) session.submit(*token, juce::Image{});
        check(awaitTerminal(session) && session.captureState() == CaptureState::Failed &&
              request.destination.loadFileAsString() == "existing invalid sentinel", "invalid frame preserves destination");
    }
    for (int invalid = 0; invalid < 7; ++invalid) {
        auto request = requestFor("invalid-request.gif");
        if (invalid == 0) request.includeAudio = true;
        if (invalid == 1) request.format = CaptureFormat::Mp4;
        if (invalid == 2) request.format = CaptureFormat::Png;
        if (invalid == 3) request.seconds = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 4) request.seconds = 16;
        if (invalid == 5) request.framing = static_cast<CaptureFraming>(99);
        if (invalid == 6) request.destination = directory.getChildFile("invalid-extension.mp4");
        GifCaptureSession session(request);
        check(!session.ready() && !session.startError().empty() && !request.destination.exists(),
              "invalid format, audio, duration, framing or destination rejected before capture");
    }

    // Real bridge restart releases old buffers and assigns a new request epoch.
    {
        auto processor = std::make_unique<CircuitDriftProcessor>(Edition::Full, "CircuitDrift");
        ProcessorEditorBridge bridge(*processor, std::filesystem::u8path(directory.getFullPathName().toStdString()));
        auto request = requestFor("bridge-first.gif");
        request.seconds = 15; // Stop explicitly; no real-duration sleep is needed.
        juce::String error;
        check(bridge.startCapture(request, error), "real bridge starts silent GIF capture");
        const auto oldToken = bridge.takeCaptureFrameRequest();
        check(oldToken.has_value(), "first bridge session issues token");
        if (oldToken) bridge.submitCaptureFrame(*oldToken, redSquare);
        check(!bridge.startCapture(request, error) && !error.isEmpty(), "active capture cannot be replaced");
        bridge.stopCapture();
        check(await([&] { return terminal(bridge.captureStatus().state); }) &&
              bridge.captureStatus().state == CaptureState::Completed, "first bridge capture completes after manual stop");
        request.destination = directory.getChildFile("bridge-restart.gif");
        check(bridge.startCapture(request, error), "terminal bridge session can restart");
        const auto nextToken = bridge.takeCaptureFrameRequest();
        check(nextToken && oldToken && nextToken->epoch != oldToken->epoch, "restart assigns new capture epoch");
        if (oldToken) bridge.submitCaptureFrame(*oldToken, juce::Image{});
        check(bridge.captureStatus().state == CaptureState::Recording, "stale old-epoch invalid image cannot fail new capture");
        if (nextToken) bridge.submitCaptureFrame(*nextToken, blueSquare);
        bridge.stopCapture();
        check(await([&] { return terminal(bridge.captureStatus().state); }) &&
              bridge.captureStatus().state == CaptureState::Completed, "fresh session completes after stale-token rejection");
        mediatest::DecodedGif gif;
        check(decode(request.destination, gif) && !gif.frames.empty() && solid(gif.frames.back(), blue),
              "restarted bridge saves new session pixels");
        request.format = CaptureFormat::Mp4;
        check(!bridge.startCapture(request, error) && error.contains("G07"), "bridge explicitly rejects unavailable MP4");
    }
    check(directory.deleteRecursively(), "remove isolated GIF regression files");
    return failures;
}
} // namespace cd::plugin::test::gif_capture_regressions
