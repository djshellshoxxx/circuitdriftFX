// Test-only minimal decoders proving encoder round trips: PNG (zlib stored + fixed-Huffman
// inflate, CRC/Adler verified) and GIF89a (LZW). Strict: any malformed input returns false.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "media/PngEncoder.h"

namespace mediatest {
using Bytes = std::vector<std::uint8_t>;

struct BitReader {
    const std::uint8_t* d; std::size_t n; std::size_t pos = 0; unsigned bit = 0;
    bool eof = false;
    unsigned get(unsigned count) {  // LSB-first
        unsigned v = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (pos >= n) { eof = true; return 0; }
            v |= unsigned((d[pos] >> bit) & 1u) << i;
            if (++bit == 8) { bit = 0; ++pos; }
        }
        return v;
    }
    void alignByte() { if (bit) { bit = 0; ++pos; } }
};

inline bool inflate(const std::uint8_t* data, std::size_t n, Bytes& out) {
    BitReader br{data, n};
    static const unsigned lbase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const unsigned lext[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const unsigned dbase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const unsigned dext[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        const unsigned final = br.get(1), type = br.get(2);
        if (br.eof) return false;
        if (type == 0) {
            br.alignByte();
            if (br.pos + 4 > n) return false;
            const unsigned len = data[br.pos] | (data[br.pos + 1] << 8), nlen = data[br.pos + 2] | (data[br.pos + 3] << 8);
            if ((len ^ 0xFFFFu) != nlen) return false;
            br.pos += 4;
            if (br.pos + len > n) return false;
            out.insert(out.end(), data + br.pos, data + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            for (;;) {
                // fixed literal/length code, read MSB-first bit by bit
                unsigned code = 0, len = 0, sym = 0;
                bool ok = false;
                for (len = 1; len <= 9 && !ok; ++len) {
                    code = (code << 1) | br.get(1);
                    if (br.eof) return false;
                    if (len == 7 && code <= 23) { sym = 256 + code; ok = true; }
                    else if (len == 8 && code >= 48 && code <= 191) { sym = code - 48; ok = true; }
                    else if (len == 8 && code >= 192 && code <= 199) { sym = 280 + (code - 192); ok = true; }
                    else if (len == 9 && code >= 400 && code <= 511) { sym = 144 + (code - 400); ok = true; }
                }
                if (!ok) return false;
                if (sym < 256) out.push_back(std::uint8_t(sym));
                else if (sym == 256) break;
                else {
                    sym -= 257;
                    if (sym >= 29) return false;
                    const unsigned l = lbase[sym] + br.get(lext[sym]);
                    unsigned dc = 0;
                    for (int i = 0; i < 5; ++i) dc = (dc << 1) | br.get(1);
                    if (dc >= 30 || br.eof) return false;
                    const unsigned dist = dbase[dc] + br.get(dext[dc]);
                    if (dist > out.size()) return false;
                    for (unsigned i = 0; i < l; ++i) out.push_back(out[out.size() - dist]);
                }
            }
        } else return false;  // dynamic Huffman not needed for our encoder
        if (final) return true;
    }
}

inline std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }

struct DecodedPng { std::uint32_t w = 0, h = 0; Bytes rgba; };
inline bool decodePng(const Bytes& f, DecodedPng& out) {
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (f.size() < 8 || !std::equal(sig, sig + 8, f.begin())) return false;
    std::size_t p = 8; Bytes idat; bool ihdr = false, iend = false;
    int depth = 0, ctype = -1;
    while (p + 12 <= f.size() && !iend) {
        const std::uint32_t len = be32(&f[p]);
        if (len > f.size() || p + 12 + len > f.size()) return false;
        if (cd::media::crc32(&f[p + 4], 4 + len) != be32(&f[p + 8 + len])) return false;
        const std::string type(reinterpret_cast<const char*>(&f[p + 4]), 4);
        if (type == "IHDR") {
            if (len != 13) return false;
            out.w = be32(&f[p + 8]); out.h = be32(&f[p + 12]); depth = f[p + 16]; ctype = f[p + 17]; ihdr = true;
        } else if (type == "IDAT") idat.insert(idat.end(), f.begin() + std::ptrdiff_t(p + 8), f.begin() + std::ptrdiff_t(p + 8 + len));
        else if (type == "IEND") iend = true;
        p += 12 + len;
    }
    if (!ihdr || !iend || depth != 8 || ctype != 6 || idat.size() < 6) return false;
    if (((idat[0] << 8) | idat[1]) % 31 != 0 || (idat[0] & 0x0F) != 8) return false;
    Bytes raw;
    if (!inflate(idat.data() + 2, idat.size() - 6, raw)) return false;
    if (cd::media::adler32(raw.data(), raw.size()) != be32(&idat[idat.size() - 4])) return false;
    const std::size_t row = 1 + std::size_t(out.w) * 4;
    if (raw.size() != row * out.h) return false;
    out.rgba.clear();
    for (std::uint32_t y = 0; y < out.h; ++y) {
        if (raw[y * row] != 0) return false;
        out.rgba.insert(out.rgba.end(), raw.begin() + std::ptrdiff_t(y * row + 1), raw.begin() + std::ptrdiff_t((y + 1) * row));
    }
    return true;
}

struct GifFrameOut { unsigned delayCs = 0; Bytes rgba; };
struct DecodedGif { std::uint32_t w = 0, h = 0; bool hasLoopExt = false; unsigned loops = 1; std::vector<GifFrameOut> frames; bool trailer = false; };

inline bool lzwDecode(const Bytes& packed, unsigned minCode, std::size_t expect, Bytes& out) {
    if (minCode < 2 || minCode > 8) return false;
    const unsigned clear = 1u << minCode, eoi = clear + 1;
    std::vector<std::uint16_t> prefix(4096, 0); std::vector<std::uint8_t> suffix(4096, 0), first(4096, 0);
    for (unsigned i = 0; i < clear; ++i) { suffix[i] = std::uint8_t(i); first[i] = std::uint8_t(i); }
    unsigned next = eoi + 1, size = minCode + 1; int prev = -1;
    BitReader br{packed.data(), packed.size()};
    auto emit = [&](unsigned code) {
        Bytes tmp;
        while (code >= clear) { tmp.push_back(suffix[code]); code = prefix[code]; if (tmp.size() > 4096) return false; }
        tmp.push_back(suffix[code]);
        out.insert(out.end(), tmp.rbegin(), tmp.rend());
        return true;
    };
    while (out.size() < expect) {
        const unsigned code = br.get(size);
        if (br.eof) return false;
        if (code == clear) { next = eoi + 1; size = minCode + 1; prev = -1; continue; }
        if (code == eoi) break;
        if (prev < 0) { if (code >= clear) return false; out.push_back(std::uint8_t(code)); prev = int(code); continue; }
        unsigned f;
        if (code < next) { if (!emit(code)) return false; f = first[code]; }
        else if (code == next) { f = first[unsigned(prev)]; if (!emit(unsigned(prev))) return false; out.push_back(std::uint8_t(f)); }
        else return false;
        if (next < 4096) { prefix[next] = std::uint16_t(prev); suffix[next] = std::uint8_t(f); first[next] = first[unsigned(prev)]; ++next; }
        if (next == (1u << size) && size < 12) ++size;
        prev = int(code);
    }
    return out.size() == expect;
}

inline bool decodeGif(const Bytes& f, DecodedGif& g) {
    if (f.size() < 13 || std::string(f.begin(), f.begin() + 6) != "GIF89a") return false;
    g.w = f[6] | (f[7] << 8); g.h = f[8] | (f[9] << 8);
    const unsigned packed = f[10];
    std::size_t p = 13;
    Bytes global;
    if (packed & 0x80) {
        const std::size_t n = std::size_t(3) << ((packed & 7) + 1);
        if (p + n > f.size()) return false;
        global.assign(f.begin() + std::ptrdiff_t(p), f.begin() + std::ptrdiff_t(p + n)); p += n;
    }
    unsigned delay = 0;
    while (p < f.size()) {
        const std::uint8_t b = f[p++];
        if (b == 0x3B) { g.trailer = true; return p == f.size(); }
        if (b == 0x21) {
            if (p >= f.size()) return false;
            const std::uint8_t label = f[p++];
            Bytes first; bool firstBlock = true;
            for (;;) {
                if (p >= f.size()) return false;
                const std::size_t len = f[p++];
                if (len == 0) break;
                if (p + len > f.size()) return false;
                if (firstBlock) { first.assign(f.begin() + std::ptrdiff_t(p), f.begin() + std::ptrdiff_t(p + len)); firstBlock = false; }
                else if (label == 0xFF && first.size() == 11 && std::string(first.begin(), first.end()) == "NETSCAPE2.0" && len == 3 && f[p] == 1) {
                    g.hasLoopExt = true; g.loops = f[p + 1] | (f[p + 2] << 8);
                }
                p += len;
            }
            if (label == 0xF9) { if (first.size() != 4) return false; delay = first[1] | (first[2] << 8); }
            continue;
        }
        if (b != 0x2C) return false;
        if (p + 9 > f.size()) return false;
        const unsigned x = f[p] | (f[p + 1] << 8), y = f[p + 2] | (f[p + 3] << 8), w = f[p + 4] | (f[p + 5] << 8), h = f[p + 6] | (f[p + 7] << 8);
        const unsigned lp = f[p + 8]; p += 9;
        if (x != 0 || y != 0 || w != g.w || h != g.h) return false;
        Bytes table = global;
        if (lp & 0x80) {
            const std::size_t n = std::size_t(3) << ((lp & 7) + 1);
            if (p + n > f.size()) return false;
            table.assign(f.begin() + std::ptrdiff_t(p), f.begin() + std::ptrdiff_t(p + n)); p += n;
        }
        if (table.empty() || p >= f.size()) return false;
        const unsigned minCode = f[p++];
        Bytes packedData;
        for (;;) {
            if (p >= f.size()) return false;
            const std::size_t len = f[p++];
            if (len == 0) break;
            if (p + len > f.size()) return false;
            packedData.insert(packedData.end(), f.begin() + std::ptrdiff_t(p), f.begin() + std::ptrdiff_t(p + len)); p += len;
        }
        Bytes idx;
        if (!lzwDecode(packedData, minCode, std::size_t(w) * h, idx)) return false;
        GifFrameOut fr; fr.delayCs = delay;
        for (auto i : idx) {
            if (std::size_t(i) * 3 + 2 >= table.size()) return false;
            fr.rgba.push_back(table[std::size_t(i) * 3]); fr.rgba.push_back(table[std::size_t(i) * 3 + 1]);
            fr.rgba.push_back(table[std::size_t(i) * 3 + 2]); fr.rgba.push_back(255);
        }
        g.frames.push_back(std::move(fr));
    }
    return false;  // no trailer
}
}  // namespace mediatest
