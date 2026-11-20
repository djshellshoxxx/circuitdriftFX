// CircuitDrift plugin adapter: product edition selector.
// Ownership: value type, no state. Threading: any thread.
#pragma once

namespace cd::plugin {

enum class Edition { Full, Lite };

// Registry ids with this prefix belong to LITE only (docs/planned-features/07-lite-edition.md).
inline constexpr const char* kLiteIdPrefix = "lite.";
// Wrapper bypass parameter exposed by LITE only (not part of the core registry).
inline constexpr const char* kBypassParamId = "bypass";

}  // namespace cd::plugin
