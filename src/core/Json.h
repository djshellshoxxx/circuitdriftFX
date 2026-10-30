// CircuitDrift core: strict bounded JSON (RFC 8259) for manifests and state documents.
// Rejects duplicate keys, non-finite numbers, invalid UTF-8, depth/size overflow, trailing data.
// Threading: non-audio only (allocates). Writer emits canonical round-trip doubles (%.17g).
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cd::core {

struct JsonValue {
    enum class Type : unsigned char { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;  // insertion order kept

    bool isNull() const noexcept { return type == Type::Null; }
    bool isBool() const noexcept { return type == Type::Bool; }
    bool isNumber() const noexcept { return type == Type::Number; }
    bool isString() const noexcept { return type == Type::String; }
    bool isArray() const noexcept { return type == Type::Array; }
    bool isObject() const noexcept { return type == Type::Object; }
    const JsonValue* get(std::string_view key) const noexcept;
    JsonValue* get(std::string_view key) noexcept;
    void set(std::string key, JsonValue v);  // replace or append (object only)

    static JsonValue makeNumber(double d) { JsonValue v; v.type = Type::Number; v.number = d; return v; }
    static JsonValue makeString(std::string s) { JsonValue v; v.type = Type::String; v.string = std::move(s); return v; }
    static JsonValue makeBool(bool b) { JsonValue v; v.type = Type::Bool; v.boolean = b; return v; }
    static JsonValue makeObject() { JsonValue v; v.type = Type::Object; return v; }
    static JsonValue makeArray() { JsonValue v; v.type = Type::Array; return v; }
};

struct JsonLimits {
    std::size_t maxBytes = 2621440;  // 2.5 MiB (session bound)
    int maxDepth = 20;
};

// Returns false and a message ("offset N: reason") on any violation; out is unspecified then.
bool parseJson(std::string_view text, JsonValue& out, std::string& error, const JsonLimits& lim = {});
std::string writeJson(const JsonValue& v);
// Maximum nesting depth of a value (scalars = 0).
int jsonDepth(const JsonValue& v) noexcept;

}  // namespace cd::core
