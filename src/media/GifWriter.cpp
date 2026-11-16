#include "media/GifWriter.h"
#include <algorithm>
#include <cstring>

namespace cd::media {
namespace {
constexpr int kClear = 256, kEoi = 257, kFirst = 258, kMaxCodes = 4096;
constexpr int kHashSize = 8192;
}  // namespace

GifWriter::GifWriter(ByteSink& sink, int width, int height, int delayCs, bool loop)
    : sink_(sink), w_(width), h_(height), delay_(delayCs), loop_(loop), table_(kHashSize) {
    const bool ok = width >= 1 && height >= 1 && width <= 65535 && height <= 65535 &&
                    delayCs >= 0 && delayCs <= 65535;
    if (!ok) failed_ = true;
    else idx_.resize(std::size_t(width) * std::size_t(height));
}

std::uint8_t GifWriter::quantize(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    const int ri = (r * 5 + 127) / 255, gi = (g * 6 + 127) / 255, bi = (b * 5 + 127) / 255;
    return std::uint8_t(ri * 42 + gi * 6 + bi);
}

void GifWriter::paletteColor(int index, std::uint8_t rgb[3]) noexcept {
    if (index < 0 || index >= 252) { rgb[0] = rgb[1] = rgb[2] = 0; return; }
    rgb[0] = std::uint8_t((index / 42) * 51);
    rgb[1] = std::uint8_t(((index / 6) % 7 * 255 + 3) / 6);
    rgb[2] = std::uint8_t((index % 6) * 51);
}

void GifWriter::put(const void* p, std::size_t n) {
    if (failed_) return;
    if (!sink_.write(p, n)) failed_ = true;
}

MediaError GifWriter::begin() {
    if (failed_ && idx_.empty()) return MediaError::InvalidArgument;
    if (began_) return MediaError::BadState;
    began_ = true;
    std::vector<std::uint8_t> h;
    const char* sig = "GIF89a";
    h.insert(h.end(), sig, sig + 6);
    h.push_back(std::uint8_t(w_)); h.push_back(std::uint8_t(w_ >> 8));
    h.push_back(std::uint8_t(h_)); h.push_back(std::uint8_t(h_ >> 8));
    h.push_back(0xF7);  // global table present, colour resolution 8 bits, table size 2^8
    h.push_back(0); h.push_back(0);
    for (int i = 0; i < 256; ++i) {
        std::uint8_t c[3];
        paletteColor(i, c);
        h.insert(h.end(), c, c + 3);
    }
    if (loop_) {
        const std::uint8_t ext[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
                                    0x03, 0x01, 0x00, 0x00, 0x00};  // loop count 0 = forever
        h.insert(h.end(), ext, ext + sizeof(ext));
    }
    put(h.data(), h.size());
    return failed_ ? MediaError::SinkWriteFailed : MediaError::Ok;
}

void GifWriter::flushSub() {
    if (subLen_ == 0) return;
    sub_[0] = std::uint8_t(subLen_);
    put(sub_, std::size_t(subLen_) + 1);
    subLen_ = 0;
}

void GifWriter::emitCode(int code, int size) {
    bitBuf_ |= std::uint32_t(code) << bitCnt_;
    bitCnt_ += size;
    while (bitCnt_ >= 8) {
        sub_[1 + subLen_++] = std::uint8_t(bitBuf_ & 0xFF);
        bitBuf_ >>= 8; bitCnt_ -= 8;
        if (subLen_ == 255) flushSub();
    }
}

void GifWriter::resetTable() {
    for (auto& s : table_) s.key = -1;
}

void GifWriter::encodeLzw(const std::uint8_t* idx) {
    const std::size_t n = std::size_t(w_) * std::size_t(h_);
    subLen_ = 0; bitBuf_ = 0; bitCnt_ = 0;
    resetTable();
    int size = 9, next = kFirst;
    emitCode(kClear, size);
    int prefix = idx[0];
    for (std::size_t i = 1; i < n; ++i) {
        const int c = idx[i];
        const std::int32_t key = (prefix << 8) | c;
        std::uint32_t h = (std::uint32_t(key) * 2654435761u) >> 19;  // 13-bit hash
        bool found = false;
        while (table_[h].key != -1) {
            if (table_[h].key == key) { prefix = table_[h].code; found = true; break; }
            h = (h + 1) & (kHashSize - 1);
        }
        if (found) continue;
        emitCode(prefix, size);
        if (next > (1 << size) - 1 && size < 12) ++size;  // decoder lags one entry behind
        if (next < kMaxCodes) {
            table_[h] = {key, next};
            ++next;
        } else {
            emitCode(kClear, size);
            resetTable();
            size = 9; next = kFirst;
        }
        prefix = c;
    }
    emitCode(prefix, size);
    if (next > (1 << size) - 1 && size < 12) ++size;
    emitCode(kEoi, size);
    if (bitCnt_ > 0) {
        sub_[1 + subLen_++] = std::uint8_t(bitBuf_ & 0xFF);
        bitBuf_ = 0; bitCnt_ = 0;
        if (subLen_ == 255) flushSub();
    }
    flushSub();
}

MediaError GifWriter::addFrameIndexed(const std::uint8_t* idx) {
    if (!idx) return MediaError::InvalidArgument;
    if (!began_) return MediaError::BadState;
    if (failed_) return MediaError::SinkWriteFailed;
    const std::uint8_t gce[] = {0x21, 0xF9, 0x04, 0x04, std::uint8_t(delay_), std::uint8_t(delay_ >> 8), 0x00, 0x00};
    put(gce, sizeof(gce));
    const std::uint8_t desc[] = {0x2C, 0, 0, 0, 0, std::uint8_t(w_), std::uint8_t(w_ >> 8),
                                 std::uint8_t(h_), std::uint8_t(h_ >> 8), 0x00, 8};
    put(desc, sizeof(desc));
    encodeLzw(idx);
    const std::uint8_t term = 0;
    put(&term, 1);
    if (failed_) return MediaError::SinkWriteFailed;
    ++frames_;
    return MediaError::Ok;
}

MediaError GifWriter::addFrameRgba(const std::uint8_t* rgba) {
    if (!rgba) return MediaError::InvalidArgument;
    const std::size_t n = idx_.size();
    for (std::size_t i = 0; i < n; ++i) idx_[i] = quantize(rgba[4 * i], rgba[4 * i + 1], rgba[4 * i + 2]);
    return addFrameIndexed(idx_.data());
}

MediaError GifWriter::finish() {
    if (!began_) return MediaError::BadState;
    const std::uint8_t t = 0x3B;
    put(&t, 1);
    return failed_ ? MediaError::SinkWriteFailed : MediaError::Ok;
}

}  // namespace cd::media
