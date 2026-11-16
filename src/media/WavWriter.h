// CircuitDrift media: WAV (RIFF/WAVE) writer for captured audio. Owner: cd_media. Worker only.
// Pcm24: WAVE_FORMAT_PCM 24-bit little-endian (clamped to [-1,1], round-to-nearest, scale 8388607).
// Float32: WAVE_FORMAT_IEEE_FLOAT with fact chunk. Non-finite samples are written as 0.
// Sizes are patched through ByteSink::patch() at finish(). Does not commit the sink.
#pragma once
#include "media/ByteSink.h"
#include "media/MediaTypes.h"
#include <cstdint>
#include <vector>

namespace cd::media {

enum class WavSampleFormat { Pcm24, Float32 };

class WavWriter {
public:
    WavWriter(ByteSink& sink, int channels, std::uint32_t sampleRate, WavSampleFormat fmt);
    MediaError begin();
    // interleaved float samples, frames*channels values.
    MediaError append(const float* interleaved, std::uint32_t frames);
    MediaError finish();
    std::uint64_t framesWritten() const noexcept { return frames_; }
    // Size of the header written by begin() (data starts here).
    std::uint64_t headerBytes() const noexcept { return dataOffset_; }

private:
    ByteSink& sink_;
    int ch_;
    std::uint32_t rate_;
    WavSampleFormat fmt_;
    bool began_ = false, bad_ = false;
    std::uint64_t frames_ = 0, dataBytes_ = 0, dataOffset_ = 0, factOffset_ = 0;
    std::vector<std::uint8_t> scratch_;
};

}  // namespace cd::media
