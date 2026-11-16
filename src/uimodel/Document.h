// CircuitDrift uimodel: view-model parameter document (plain values + ranges + factory bases).
// Owner: cd_uimodel. Framework-independent (no JUCE, no graphics). Message thread only (allocates).
// The document is the UI-side mirror of stored driver/base values; History blobs are its
// canonical text serialization. It never touches audio state directly.
#pragma once
#include <map>
#include <string>
#include "state/History.h"

namespace cd::uimodel {

struct Entry {
    double value = 0, min = 0, max = 1, factory = 0;
    bool discrete = false;  // bool/enum/int: values are whole numbers
};

class ParamDocument {
public:
    // Adds or replaces an entry (non-finite/inverted ranges are rejected -> false).
    bool add(const std::string& id, double min, double max, double factory, bool discrete = false);
    // Adds every registry parameter (plain units, factory = registry default).
    static ParamDocument fromRegistry();
    const Entry* find(const std::string& id) const noexcept;
    double get(const std::string& id) const noexcept;  // NaN when unknown
    // Clamps into [min,max] (discrete: rounded); non-finite or unknown -> false, no change.
    bool set(const std::string& id, double plain);
    double normalized(const std::string& id) const noexcept;
    std::string serialize() const;                   // canonical "id=%.17g\n" lines (sorted)
    bool restore(const std::string& blob);           // all-or-nothing
    state::ConfigBlob blob() const { return state::History::makeBlob(serialize()); }
    const std::map<std::string, Entry>& entries() const noexcept { return e_; }
private:
    std::map<std::string, Entry> e_;
};

}  // namespace cd::uimodel
