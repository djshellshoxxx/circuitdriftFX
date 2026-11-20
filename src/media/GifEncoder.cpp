#include "media/GifEncoder.h"
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>

namespace cd::media {
namespace {
struct Entry { std::uint32_t key; std::uint32_t count; };
static_assert(sizeof(Entry) == 8);
constexpr std::size_t kPaletteBytes = 256 * 3;
constexpr std::size_t kFixedWorkingBytes = 128 * 1024;

std::uint32_t packRgb(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2]; }

// Sorted unique colours with counts.
std::vector<Entry> uniqueColours(const std::uint8_t* rgba, std::size_t n) {
    std::vector<std::uint32_t> keys(n);
    for (std::size_t i = 0; i < n; ++i) keys[i] = packRgb(rgba + i * 4);
    std::sort(keys.begin(), keys.end());
    std::vector<Entry> u;
    u.reserve(n);  // Bound histogram capacity; never retain old/new geometric allocations.
    for (std::size_t i = 0; i < n;) {
        std::size_t j = i;
        while (j < n && keys[j] == keys[i]) ++j;
        u.push_back({keys[i], std::uint32_t(j - i)});
        i = j;
    }
    return u;
}

int channel(std::uint32_t key, int c) { return int((key >> (16 - 8 * c)) & 0xFFu); }

// Median cut over unique colours; returns box id per unique colour and the palette.
void medianCut(const std::vector<Entry>& u, unsigned maxColors, std::vector<std::uint8_t>& palette,
               std::vector<std::uint16_t>& boxOf) {
    struct Box { std::size_t lo, hi; };  // range in `order`
    std::vector<std::size_t> order(u.size());
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::vector<Box> boxes;
    boxes.reserve(maxColors);
    boxes.push_back({0, u.size()});
    auto range = [&](const Box& b, int& axis) {
        int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
        for (std::size_t i = b.lo; i < b.hi; ++i)
            for (int c = 0; c < 3; ++c) {
                const int v = channel(u[order[i]].key, c);
                lo[c] = std::min(lo[c], v); hi[c] = std::max(hi[c], v);
            }
        int best = -1; axis = 0;
        for (int c = 0; c < 3; ++c)
            if (hi[c] - lo[c] > best) { best = hi[c] - lo[c]; axis = c; }  // ties: lowest channel index
        return best;
    };
    while (boxes.size() < maxColors) {
        std::size_t pick = boxes.size();
        int bestRange = 0, bestAxis = 0;
        for (std::size_t b = 0; b < boxes.size(); ++b) {
            if (boxes[b].hi - boxes[b].lo < 2) continue;
            int axis;
            const int r = range(boxes[b], axis);
            if (r > bestRange) { bestRange = r; bestAxis = axis; pick = b; }
        }
        if (pick == boxes.size()) break;
        Box box = boxes[pick];
        std::sort(order.begin() + std::ptrdiff_t(box.lo), order.begin() + std::ptrdiff_t(box.hi),
                  [&](std::size_t a, std::size_t b) {
                      const int ca = channel(u[a].key, bestAxis), cb = channel(u[b].key, bestAxis);
                      return ca != cb ? ca < cb : u[a].key < u[b].key;
                  });
        // Split at the count-weighted median, keeping both halves non-empty.
        std::uint64_t total = 0;
        for (std::size_t i = box.lo; i < box.hi; ++i) total += u[order[i]].count;
        std::uint64_t acc = 0;
        std::size_t mid = box.lo + 1;
        for (std::size_t i = box.lo; i + 1 < box.hi; ++i) {
            acc += u[order[i]].count;
            mid = i + 1;
            if (acc * 2 >= total) break;
        }
        boxes[pick] = {box.lo, mid};
        boxes.push_back({mid, box.hi});
    }
    palette.assign(boxes.size() * 3, 0);
    boxOf.assign(u.size(), 0);
    for (std::size_t b = 0; b < boxes.size(); ++b) {
        std::uint64_t sum[3] = {0, 0, 0}, total = 0;
        for (std::size_t i = boxes[b].lo; i < boxes[b].hi; ++i) {
            const Entry& e = u[order[i]];
            for (int c = 0; c < 3; ++c) sum[c] += std::uint64_t(channel(e.key, c)) * e.count;
            total += e.count;
            boxOf[order[i]] = std::uint16_t(b);
        }
        for (int c = 0; c < 3; ++c) palette[b * 3 + std::size_t(c)] = std::uint8_t((sum[c] + total / 2) / total);
    }
}

std::size_t findKey(const std::vector<Entry>& u, std::uint32_t key) {
    const auto it = std::lower_bound(u.begin(), u.end(), key, [](const Entry& e, std::uint32_t k) { return e.key < k; });
    return std::size_t(it - u.begin());
}

unsigned bitsFor(std::size_t colours) {
    unsigned b = 1;
    while ((std::size_t(1) << b) < colours) ++b;
    return b;  // table has 2^b entries, b in [1,8]
}

struct OutputLimitExceeded {};

// begin() reserves the entire output budget, so checked appends neither exceed
// the cap nor create a transient old/new allocation pair while encoding.
class BoundedOutput {
public:
    BoundedOutput(std::vector<std::uint8_t>& bytes, std::size_t limit) : bytes_(bytes), limit_(limit) {}
    void push_back(std::uint8_t byte) {
        admit(1);
        bytes_.push_back(byte);
    }
    template <typename It>
    void append(It first, std::size_t count) {
        admit(count);
        const auto offset = bytes_.size();
        bytes_.resize(offset + count);
        std::copy_n(first, count, bytes_.begin() + std::ptrdiff_t(offset));
    }
private:
    void admit(std::size_t count) const {
        if (bytes_.size() > limit_ || count > limit_ - bytes_.size()) throw OutputLimitExceeded{};
    }
    std::vector<std::uint8_t>& bytes_;
    std::size_t limit_;
};

void le16(BoundedOutput& v, std::uint32_t x) { v.push_back(std::uint8_t(x)); v.push_back(std::uint8_t(x >> 8)); }

class BitPacker {
public:
    explicit BitPacker(BoundedOutput& out) : out_(out) {}
    void put(unsigned code, unsigned size) {
        acc_ |= std::uint32_t(code) << n_;
        n_ += size;
        while (n_ >= 8) { block_[len_++] = std::uint8_t(acc_); acc_ >>= 8; n_ -= 8; if (len_ == 255) flushBlock(); }
    }
    void finish() {
        if (n_ > 0) { block_[len_++] = std::uint8_t(acc_); acc_ = 0; n_ = 0; if (len_ == 255) flushBlock(); }
        flushBlock();
        out_.push_back(0);  // block terminator
    }
private:
    void flushBlock() {
        if (!len_) return;
        out_.push_back(std::uint8_t(len_));
        out_.append(block_.begin(), len_);
        len_ = 0;
    }
    BoundedOutput& out_;
    std::array<std::uint8_t, 255> block_{};
    std::size_t len_ = 0;
    std::uint32_t acc_ = 0;
    unsigned n_ = 0;
};

void lzwEncode(const std::vector<std::uint8_t>& idx, unsigned minCode, BoundedOutput& out) {
    out.push_back(std::uint8_t(minCode));
    BitPacker bp(out);
    const unsigned clear = 1u << minCode, eoi = clear + 1;
    unsigned next = eoi + 1, size = minCode + 1;
    constexpr unsigned kTable = 8192;  // open addressing, load <= 0.5
    std::vector<std::int32_t> keys(kTable, -1);
    std::vector<std::uint16_t> vals(kTable, 0);
    auto reset = [&] { std::fill(keys.begin(), keys.end(), -1); next = eoi + 1; size = minCode + 1; };
    bp.put(clear, size);
    if (idx.empty()) { bp.put(eoi, size); bp.finish(); return; }
    unsigned prefix = idx[0];
    for (std::size_t i = 1; i < idx.size(); ++i) {
        const std::int32_t key = std::int32_t(prefix * 256u + idx[i]);
        unsigned slot = (unsigned(key) * 2654435761u) >> 19 & (kTable - 1);
        bool found = false;
        while (keys[slot] != -1) {
            if (keys[slot] == key) { found = true; break; }
            slot = (slot + 1) & (kTable - 1);
        }
        if (found) { prefix = vals[slot]; continue; }
        bp.put(prefix, size);
        keys[slot] = key; vals[slot] = std::uint16_t(next);
        ++next;
        if (next > (1u << size) && size < 12) ++size;
        if (next == 4096) { bp.put(clear, size); reset(); }
        prefix = idx[i];
    }
    bp.put(prefix, size);
    // The decoder adds an entry after reading this last code and may widen before the EOI code;
    // the encoder's table runs one entry ahead of the decoder's, hence the equality test.
    if (next == (1u << size) && size < 12) ++size;
    bp.put(eoi, size);
    bp.finish();
}
}  // namespace

QuantizedFrame quantizeRgba(const std::uint8_t* rgba, std::size_t pixelCount, unsigned maxColors) {
    QuantizedFrame q;
    q.palette.reserve(kPaletteBytes);
    maxColors = std::clamp(maxColors, 2u, 256u);
    const auto u = uniqueColours(rgba, pixelCount);
    std::vector<std::uint16_t> boxOf(u.size(), 0);
    if (u.size() <= maxColors) {
        q.exact = true;
        q.palette.resize(u.size() * 3);
        for (std::size_t i = 0; i < u.size(); ++i) {
            for (int c = 0; c < 3; ++c) q.palette[i * 3 + std::size_t(c)] = std::uint8_t(channel(u[i].key, c));
            boxOf[i] = std::uint16_t(i);
        }
    } else {
        medianCut(u, maxColors, q.palette, boxOf);
    }
    q.indices.resize(pixelCount);
    for (std::size_t i = 0; i < pixelCount; ++i) q.indices[i] = std::uint8_t(boxOf[findKey(u, packRgb(rgba + i * 4))]);
    return q;
}

std::size_t gifEncodingMemoryBound(std::uint32_t width, std::uint32_t height, std::size_t outputBytes) noexcept {
    constexpr auto overflow = std::numeric_limits<std::size_t>::max();
    if (width == 0 || height == 0 || width > 65535 || height > 65535 || outputBytes == 0 ||
        outputBytes > std::size_t(std::numeric_limits<std::ptrdiff_t>::max()) ||
        std::size_t(width) > overflow / height) return overflow;
    const std::size_t pixels = std::size_t(width) * height;
    if (pixels > overflow / 4) return overflow;  // Caller RGBA byte count must also be representable.
    // Histogram construction: keys 4N + entries 8N. Median cut: entries 8N +
    // box IDs 2N + order sizeof(size_t)N. Indices are allocated after order dies
    // (8N + 2N + N). Reserve all variable-capacity containers explicitly.
    constexpr auto scratchPerPixel = std::max(std::size_t(12), std::size_t(10) + sizeof(std::size_t));
    if (outputBytes > overflow - kFixedWorkingBytes ||
        pixels > (overflow - outputBytes - kFixedWorkingBytes) / scratchPerPixel) return overflow;
    // Fixed allowance covers the 8192*(4+2) LZW table bytes, <=256 median-cut
    // boxes, palettes, remap, bit-packer, vector objects and ordinary sort stack.
    return outputBytes + pixels * scratchPerPixel + kFixedWorkingBytes;
}

bool GifWriter::begin(std::uint32_t width, std::uint32_t height, const GifOptions& opt, const GifLimits& lim, std::string& error) {
    if (begun_) { error = "GIF writer already begun"; return false; }
    if (width == 0 || height == 0 || width > lim.maxDimension || height > lim.maxDimension || lim.maxDimension > 65535) {
        error = "GIF dimensions out of range"; return false;
    }
    if (lim.maxFrames == 0) { error = "GIF frame cap is zero"; return false; }
    if (gifEncodingMemoryBound(width, height, lim.maxBytes) == std::numeric_limits<std::size_t>::max() ||
        lim.maxBytes > buf_.max_size()) { error = "GIF storage limit out of range"; return false; }
    try {
        buf_.reserve(lim.maxBytes);
        global_.reserve(kPaletteBytes);
    } catch (const std::bad_alloc&) {
        std::vector<std::uint8_t>().swap(buf_);
        error = "GIF storage allocation failed"; return false;
    } catch (const std::length_error&) {
        std::vector<std::uint8_t>().swap(buf_);
        error = "GIF storage limit out of range"; return false;
    }
    // Supported vector implementations reserve an empty vector exactly. Refuse
    // any larger capacity rather than silently violating the admitted payload.
    if (buf_.capacity() > lim.maxBytes) {
        std::vector<std::uint8_t>().swap(buf_);
        error = "GIF output capacity exceeds budget"; return false;
    }
    w_ = width; h_ = height; opt_ = opt; lim_ = lim; begun_ = true;
    error.clear();
    return true;
}

bool GifWriter::addFrame(const std::uint8_t* rgba, std::size_t rgbaBytes, std::string& error) {
    if (!begun_ || finished_ || failed_) { error = "GIF writer not accepting frames"; return false; }
    if (frames_ >= lim_.maxFrames) { error = "GIF frame cap reached"; return false; }
    const std::size_t pixels = std::size_t(w_) * h_;
    if (!rgba || rgbaBytes != pixels * 4) { error = "frame buffer size does not match dimensions"; return false; }
    const auto originalSize = buf_.size();
    auto fail = [&](const char* why) {
        buf_.resize(originalSize);  // Discard any partially encoded frame; capacity never grows.
        failed_ = true;
        error = why;
        return false;
    };
    if (buf_.size() >= lim_.maxBytes) return fail("GIF size cap reached");

    try {
        QuantizedFrame q = quantizeRgba(rgba, pixels);
        const bool first = frames_ == 0;
        bool useGlobal = false;
        if (!first) {
            // Reuse the global table if every colour of this frame is present in it (exact only).
            std::vector<std::uint8_t> remap(q.palette.size() / 3);
            bool all = q.exact;
            const std::size_t gn = global_.size() / 3;
            for (std::size_t i = 0; all && i < remap.size(); ++i) {
                bool hit = false;
                for (std::size_t g = 0; g < gn; ++g)
                    if (std::equal(q.palette.begin() + std::ptrdiff_t(i * 3), q.palette.begin() + std::ptrdiff_t(i * 3 + 3), global_.begin() + std::ptrdiff_t(g * 3))) {
                        remap[i] = std::uint8_t(g); hit = true; break;
                    }
                all = hit;
            }
            if (all) {
                for (auto& v : q.indices) v = remap[v];
                useGlobal = true;
            }
        }
        const std::size_t colours = q.palette.size() / 3;
        const unsigned bits = bitsFor(colours);
        q.palette.resize((std::size_t(1) << bits) * 3, 0);

        BoundedOutput o(buf_, lim_.maxBytes);
        if (first) {
            global_ = q.palette; globalBits_ = bits;
            const char hdr[] = "GIF89a";
            o.append(hdr, 6);
            le16(o, w_); le16(o, h_);
            o.push_back(std::uint8_t(0x80 | (7 << 4) | (bits - 1)));
            o.push_back(0); o.push_back(0);
            o.append(global_.begin(), global_.size());
            if (opt_.loop) {
                o.push_back(0x21); o.push_back(0xFF); o.push_back(11);
                const char app[] = "NETSCAPE2.0";
                o.append(app, 11);
                o.push_back(3); o.push_back(1); le16(o, 0); o.push_back(0);  // loop forever
            }
            useGlobal = true;
        }
        const auto imageOffset = buf_.size();
        // Graphic control extension.
        o.push_back(0x21); o.push_back(0xF9); o.push_back(4);
        o.push_back(1 << 2);  // disposal: do not dispose, no transparency
        le16(o, opt_.delayCentiseconds);
        o.push_back(0); o.push_back(0);
        // Image descriptor.
        o.push_back(0x2C); le16(o, 0); le16(o, 0); le16(o, w_); le16(o, h_);
        const unsigned codeBits = useGlobal ? globalBits_ : bits;
        if (useGlobal) o.push_back(0);
        else { o.push_back(std::uint8_t(0x80 | (bits - 1))); o.append(q.palette.begin(), q.palette.size()); }
        lzwEncode(q.indices, std::max(2u, codeBits), o);
        lastFrameOffset_ = imageOffset;
        lastFrameBytes_ = buf_.size() - imageOffset;
        ++frames_;
        error.clear();
        return true;
    } catch (const OutputLimitExceeded&) {
        return fail("GIF size cap exceeded");
    } catch (const std::bad_alloc&) {
        return fail("GIF working storage allocation failed");
    } catch (const std::length_error&) {
        return fail("GIF working storage limit out of range");
    }
}

bool GifWriter::repeatLastFrame(std::string& error) {
    if (!begun_ || finished_ || failed_ || frames_ == 0) { error = "GIF has no frame to repeat"; return false; }
    if (frames_ >= lim_.maxFrames) { error = "GIF frame cap reached"; return false; }
    if (lastFrameBytes_ > lim_.maxBytes - buf_.size()) {
        failed_ = true;
        error = "GIF size cap exceeded";
        return false;
    }
    const auto offset = buf_.size();
    buf_.resize(offset + lastFrameBytes_);  // Within the pre-reserved output capacity.
    // Self-range vector::insert is invalid; resize first, then copy disjoint ranges.
    std::copy_n(buf_.begin() + std::ptrdiff_t(lastFrameOffset_), lastFrameBytes_,
                buf_.begin() + std::ptrdiff_t(offset));
    lastFrameOffset_ = offset;
    ++frames_;
    error.clear();
    return true;
}

bool GifWriter::finish(std::vector<std::uint8_t>& out, std::string& error) {
    if (!begun_ || finished_ || failed_ || frames_ == 0) { error = "GIF has no frames or failed"; return false; }
    if (buf_.size() >= lim_.maxBytes) { failed_ = true; error = "GIF size cap excludes trailer"; return false; }
    buf_.push_back(0x3B);
    finished_ = true;
    out = std::move(buf_);
    error.clear();
    return true;
}

}  // namespace cd::media
