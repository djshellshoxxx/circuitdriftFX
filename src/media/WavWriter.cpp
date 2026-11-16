#include "media/WavWriter.h"
#include <cmath>
#include <cstring>

namespace cd::media {
namespace {
void le16(std::vector<std::uint8_t>& v, std::uint32_t x) { v.push_back(std::uint8_t(x)); v.push_back(std::uint8_t(x >> 8)); }
void le32(std::vector<std::uint8_t>& v, std::uint32_t x) { le16(v, x & 0xFFFF); le16(v, x >> 16); }
void tag(std::vector<std::uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }
constexpr std::uint32_t kChunkFrames = 4096;
}  // namespace

WavWriter::WavWriter(ByteSink& sink, int channels, std::uint32_t sampleRate, WavSampleFormat fmt)
    : sink_(sink), ch_(channels), rate_(sampleRate), fmt_(fmt) {
    if (channels < 1 || channels > 8 || sampleRate < 1000 || sampleRate > 768000) bad_ = true;
}

MediaError WavWriter::begin() {
    if (bad_) return MediaError::InvalidArgument;
    if (began_) return MediaError::BadState;
    began_ = true;
    const bool flt = fmt_ == WavSampleFormat::Float32;
    const std::uint32_t bytesPer = flt ? 4 : 3;
    std::vector<std::uint8_t> h;
    tag(h, "RIFF"); le32(h, 0); tag(h, "WAVE");
    tag(h, "fmt "); le32(h, flt ? 18 : 16);
    le16(h, flt ? 3 : 1); le16(h, std::uint32_t(ch_)); le32(h, rate_);
    le32(h, rate_ * bytesPer * std::uint32_t(ch_)); le16(h, bytesPer * std::uint32_t(ch_)); le16(h, bytesPer * 8);
    if (flt) {
        le16(h, 0);  // cbSize
        tag(h, "fact"); le32(h, 4);
        factOffset_ = h.size();
        le32(h, 0);
    }
    tag(h, "data"); le32(h, 0);
    dataOffset_ = h.size();
    return sink_.write(h.data(), h.size()) ? MediaError::Ok : MediaError::SinkWriteFailed;
}

MediaError WavWriter::append(const float* in, std::uint32_t frames) {
    if (!began_) return MediaError::BadState;
    if (!in && frames) return MediaError::InvalidArgument;
    const std::size_t bytesPer = fmt_ == WavSampleFormat::Float32 ? 4 : 3;
    while (frames > 0) {
        const std::uint32_t n = frames < kChunkFrames ? frames : kChunkFrames;
        const std::size_t count = std::size_t(n) * std::size_t(ch_);
        if (dataBytes_ + count * bytesPer > 0xFFFFFFFFull - (dataOffset_ - 8) - 1) return MediaError::InvalidArgument;  // RIFF size field must not wrap
        scratch_.resize(count * bytesPer);  // capacity settles after first chunk
        std::uint8_t* o = scratch_.data();
        for (std::size_t i = 0; i < count; ++i) {
            float x = in[i];
            if (!std::isfinite(x)) x = 0.0f;
            if (fmt_ == WavSampleFormat::Float32) {
                std::uint32_t b;
                std::memcpy(&b, &x, 4);
                o[0] = std::uint8_t(b); o[1] = std::uint8_t(b >> 8); o[2] = std::uint8_t(b >> 16); o[3] = std::uint8_t(b >> 24);
                o += 4;
            } else {
                if (x > 1.0f) x = 1.0f;
                if (x < -1.0f) x = -1.0f;
                const std::int32_t v = std::int32_t(std::lround(double(x) * 8388607.0));
                o[0] = std::uint8_t(v); o[1] = std::uint8_t(v >> 8); o[2] = std::uint8_t(v >> 16);
                o += 3;
            }
        }
        if (!sink_.write(scratch_.data(), scratch_.size())) return MediaError::SinkWriteFailed;
        dataBytes_ += scratch_.size();
        frames_ += n;
        in += count; frames -= n;
    }
    return MediaError::Ok;
}

MediaError WavWriter::finish() {
    if (!began_) return MediaError::BadState;
    if (dataBytes_ & 1) {  // RIFF chunks are word aligned
        const std::uint8_t pad = 0;
        if (!sink_.write(&pad, 1)) return MediaError::SinkWriteFailed;
    }
    std::vector<std::uint8_t> v;
    le32(v, std::uint32_t(dataBytes_));
    const std::uint64_t total = dataOffset_ + dataBytes_ + (dataBytes_ & 1);
    std::vector<std::uint8_t> r;
    le32(r, std::uint32_t(total - 8));
    bool ok = sink_.patch(4, r.data(), 4) && sink_.patch(dataOffset_ - 4, v.data(), 4);
    if (fmt_ == WavSampleFormat::Float32) {
        std::vector<std::uint8_t> f;
        le32(f, std::uint32_t(frames_));
        ok = ok && sink_.patch(factOffset_, f.data(), 4);
    }
    return ok ? MediaError::Ok : MediaError::SinkWriteFailed;
}

}  // namespace cd::media
