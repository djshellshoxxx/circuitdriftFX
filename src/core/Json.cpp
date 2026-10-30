#include "core/Json.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cd::core {

const JsonValue* JsonValue::get(std::string_view key) const noexcept {
    for (const auto& kv : object) if (kv.first == key) return &kv.second;
    return nullptr;
}
JsonValue* JsonValue::get(std::string_view key) noexcept {
    for (auto& kv : object) if (kv.first == key) return &kv.second;
    return nullptr;
}
void JsonValue::set(std::string key, JsonValue v) {
    if (JsonValue* e = get(key)) *e = std::move(v);
    else object.emplace_back(std::move(key), std::move(v));
}

int jsonDepth(const JsonValue& v) noexcept {
    int d = 0;
    for (const auto& e : v.array) d = std::max(d, jsonDepth(e));
    for (const auto& kv : v.object) d = std::max(d, jsonDepth(kv.second));
    return (v.isArray() || v.isObject()) ? d + 1 : 0;
}

namespace {
struct Parser {
    std::string_view s;
    std::size_t i = 0;
    int maxDepth;
    std::string err;

    bool fail(const char* why) {
        if (err.empty()) err = "offset " + std::to_string(i) + ": " + why;
        return false;
    }
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool lit(std::string_view w) {
        if (s.substr(i, w.size()) != w) return fail("invalid literal");
        i += w.size();
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
        else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return fail("short \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return fail("bad hex digit");
        }
        return true;
    }
    bool str(std::string& o) {
        ++i;  // opening quote
        while (true) {
            if (i >= s.size()) return fail("unterminated string");
            const unsigned char c = (unsigned char)s[i];
            if (c == '"') { ++i; return true; }
            if (c < 0x20) return fail("control character in string");
            if (c == '\\') {
                if (++i >= s.size()) return fail("unterminated escape");
                const char e = s[i++];
                switch (e) {
                    case '"': o += '"'; break;  case '\\': o += '\\'; break; case '/': o += '/'; break;
                    case 'b': o += '\b'; break; case 'f': o += '\f'; break;  case 'n': o += '\n'; break;
                    case 'r': o += '\r'; break; case 't': o += '\t'; break;
                    case 'u': {
                        unsigned cp;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone low surrogate");
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            unsigned lo;
                            if (s.substr(i, 2) != "\\u") return fail("lone high surrogate");
                            i += 2;
                            if (!hex4(lo)) return false;
                            if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(o, cp);
                        break;
                    }
                    default: return fail("invalid escape");
                }
                continue;
            }
            // Validate one UTF-8 sequence (no overlongs, no surrogates, <= U+10FFFF).
            int len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
            if (!len || i + len > s.size()) return fail("invalid UTF-8");
            unsigned cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
            for (int k = 1; k < len; ++k) {
                const unsigned char d = (unsigned char)s[i + k];
                if ((d & 0xC0) != 0x80) return fail("invalid UTF-8");
                cp = (cp << 6) | (d & 0x3F);
            }
            if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
                return fail("invalid UTF-8");
            o.append(s.data() + i, len);
            i += len;
        }
    }
    bool num(double& out) {
        const std::size_t b = i;
        auto digit = [&] { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
        if (i < s.size() && s[i] == '-') ++i;
        if (!digit()) return fail("invalid number");
        if (s[i] == '0') { ++i; if (digit()) return fail("leading zero"); }
        else while (digit()) ++i;
        if (i < s.size() && s[i] == '.') { ++i; if (!digit()) return fail("invalid fraction"); while (digit()) ++i; }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (!digit()) return fail("invalid exponent");
            while (digit()) ++i;
        }
        const std::string tok(s.substr(b, i - b));
        out = std::strtod(tok.c_str(), nullptr);
        if (!std::isfinite(out)) return fail("non-finite number");
        return true;
    }
    bool value(JsonValue& v, int depth) {
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{' || c == '[') {
            if (depth >= maxDepth) return fail("nesting too deep");
            ++i;
            const bool obj = c == '{';
            v.type = obj ? JsonValue::Type::Object : JsonValue::Type::Array;
            ws();
            if (i < s.size() && s[i] == (obj ? '}' : ']')) { ++i; return true; }
            while (true) {
                if (obj) {
                    ws();
                    if (i >= s.size() || s[i] != '"') return fail("expected key");
                    std::string key;
                    if (!str(key)) return false;
                    if (v.get(key)) return fail("duplicate key");
                    ws();
                    if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                    ++i;
                    JsonValue child;
                    if (!value(child, depth + 1)) return false;
                    v.object.emplace_back(std::move(key), std::move(child));
                } else {
                    JsonValue child;
                    if (!value(child, depth + 1)) return false;
                    v.array.push_back(std::move(child));
                }
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == (obj ? '}' : ']')) { ++i; return true; }
                return fail("expected ',' or close");
            }
        }
        if (c == '"') { v.type = JsonValue::Type::String; return str(v.string); }
        if (c == 't') { v.type = JsonValue::Type::Bool; v.boolean = true; return lit("true"); }
        if (c == 'f') { v.type = JsonValue::Type::Bool; v.boolean = false; return lit("false"); }
        if (c == 'n') { v.type = JsonValue::Type::Null; return lit("null"); }
        v.type = JsonValue::Type::Number;
        return num(v.number);
    }
};

void writeString(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    o += '"';
}

void write(std::string& o, const JsonValue& v) {
    switch (v.type) {
        case JsonValue::Type::Null: o += "null"; break;
        case JsonValue::Type::Bool: o += v.boolean ? "true" : "false"; break;
        case JsonValue::Type::Number: {
            char b[32];
            std::snprintf(b, sizeof b, "%.17g", v.number);
            o += b;
            break;
        }
        case JsonValue::Type::String: writeString(o, v.string); break;
        case JsonValue::Type::Array:
            o += '[';
            for (std::size_t k = 0; k < v.array.size(); ++k) { if (k) o += ','; write(o, v.array[k]); }
            o += ']';
            break;
        case JsonValue::Type::Object:
            o += '{';
            for (std::size_t k = 0; k < v.object.size(); ++k) {
                if (k) o += ',';
                writeString(o, v.object[k].first);
                o += ':';
                write(o, v.object[k].second);
            }
            o += '}';
            break;
    }
}
}  // namespace

bool parseJson(std::string_view text, JsonValue& out, std::string& error, const JsonLimits& lim) {
    error.clear();
    if (text.size() > lim.maxBytes) { error = "document exceeds size bound"; return false; }
    Parser p{text, 0, lim.maxDepth, {}};
    out = JsonValue{};
    if (!p.value(out, 0)) { error = p.err; return false; }
    p.ws();
    if (p.i != text.size()) { p.fail("trailing data"); error = p.err; return false; }
    return true;
}

std::string writeJson(const JsonValue& v) {
    std::string o;
    write(o, v);
    return o;
}

}  // namespace cd::core
