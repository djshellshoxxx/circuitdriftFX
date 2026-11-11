// CircuitDrift media: video encoder interface and availability probe (P13). Owner: cd_media.
// Threading: encoders are driven only by the capture worker thread. They own their ByteSink
// reference (commit/abort delegate to it). Gate G07 (native MP4 encoders: Windows Media
// Foundation / macOS AVAssetWriter) is OPEN: probeEncoder(Mp4) reports unavailable with a reason
// and makeVideoEncoder(Mp4) returns null. Nothing here fakes MP4/H.264 or silently substitutes a
// different format.
#pragma once
#include "media/ByteSink.h"
#include "media/MediaTypes.h"
#include <cstdint>
#include <memory>
#include <string>

namespace cd::media {

struct EncoderAvailability {
    bool available = false;
    std::string reason;  // empty when available
};

EncoderAvailability probeEncoder(Format f);

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    virtual const char* name() const noexcept = 0;
    virtual double maxDurationSec() const noexcept = 0;  // spec: GIF 15 s, MP4 60 s
    virtual int maxFrames() const noexcept = 0;          // 0 = unlimited; PNG = 1
    virtual MediaError begin() = 0;
    virtual MediaError addFrame(const std::uint8_t* rgba) = 0;  // exact selected dimensions
    virtual MediaError finish() = 0;
    virtual bool commit() = 0;
    virtual void abort() noexcept = 0;
};

// Png: one still image. Gif: width x height, delay = 100/fps centiseconds (fps must divide 100;
// 20 FPS -> 5 cs = 50 ms), loop optional. Mp4/other: returns null with error Unavailable.
std::unique_ptr<IVideoEncoder> makeVideoEncoder(Format f, ByteSink& sink, int width, int height, int fps,
                                                bool loop, MediaError* err, std::string* reason);

}  // namespace cd::media
