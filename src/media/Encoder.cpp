#include "media/Encoder.h"
#include "media/GifWriter.h"
#include "media/PngWriter.h"

namespace cd::media {
namespace {

class PngEncoder final : public IVideoEncoder {
public:
    PngEncoder(ByteSink& s, int w, int h) : sink_(s), w_(w), h_(h) {}
    const char* name() const noexcept override { return "png"; }
    double maxDurationSec() const noexcept override { return 0.0; }
    int maxFrames() const noexcept override { return 1; }
    MediaError begin() override { return MediaError::Ok; }
    MediaError addFrame(const std::uint8_t* rgba) override {
        if (done_) return MediaError::BadState;
        done_ = true;
        return writePng(sink_, rgba, w_, h_);
    }
    MediaError finish() override { return done_ ? MediaError::Ok : MediaError::NoData; }
    bool commit() override { return sink_.commit(); }
    void abort() noexcept override { sink_.abort(); }
private:
    ByteSink& sink_;
    int w_, h_;
    bool done_ = false;
};

class GifEncoder final : public IVideoEncoder {
public:
    GifEncoder(ByteSink& s, int w, int h, int delayCs, bool loop) : sink_(s), gif_(s, w, h, delayCs, loop) {}
    const char* name() const noexcept override { return "gif"; }
    double maxDurationSec() const noexcept override { return 15.0; }
    int maxFrames() const noexcept override { return 0; }
    MediaError begin() override { return gif_.begin(); }
    MediaError addFrame(const std::uint8_t* rgba) override { return gif_.addFrameRgba(rgba); }
    MediaError finish() override { return gif_.finish(); }
    bool commit() override { return sink_.commit(); }
    void abort() noexcept override { sink_.abort(); }
private:
    ByteSink& sink_;
    GifWriter gif_;
};

}  // namespace

EncoderAvailability probeEncoder(Format f) {
    switch (f) {
        case Format::Png:
        case Format::Gif: return {true, {}};
        case Format::Mp4:
            return {false,
                    "MP4/H.264 export is unavailable: native encoders (Windows Media Foundation, macOS "
                    "AVAssetWriter) are not implemented and gate G07 is open. PNG and GIF are available; "
                    "no other format is substituted automatically."};
    }
    return {false, "unknown format"};
}

std::unique_ptr<IVideoEncoder> makeVideoEncoder(Format f, ByteSink& sink, int w, int h, int fps, bool loop,
                                                MediaError* err, std::string* reason) {
    auto fail = [&](MediaError e, const std::string& why) -> std::unique_ptr<IVideoEncoder> {
        if (err) *err = e;
        if (reason) *reason = why;
        return nullptr;
    };
    const EncoderAvailability av = probeEncoder(f);
    if (!av.available) return fail(MediaError::Unavailable, av.reason);
    if (w < 1 || h < 1 || w > kMaxImageDim || h > kMaxImageDim) return fail(MediaError::InvalidArgument, "dimensions out of range");
    if (err) *err = MediaError::Ok;
    if (f == Format::Png) return std::make_unique<PngEncoder>(sink, w, h);
    if (fps < 1 || fps > 100 || 100 % fps != 0 || w > 65535 || h > 65535)
        return fail(MediaError::InvalidArgument, "GIF fps must divide 100 (frame delay in whole centiseconds)");
    return std::make_unique<GifEncoder>(sink, w, h, 100 / fps, loop);
}

}  // namespace cd::media
