// CircuitDrift uimodel: ParamDocument implementation. Owner: cd_uimodel. Message thread only.
#include "uimodel/Document.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "core/ParameterRegistry.h"

namespace cd::uimodel {

bool ParamDocument::add(const std::string& id, double min, double max, double factory, bool discrete) {
    if (id.empty() || !std::isfinite(min) || !std::isfinite(max) || !std::isfinite(factory) || !(max > min)) return false;
    Entry e;
    e.min = min; e.max = max; e.discrete = discrete;
    e.factory = e.value = std::fmin(max, std::fmax(min, factory));
    e_[id] = e;
    return true;
}

ParamDocument ParamDocument::fromRegistry() {
    ParamDocument d;
    for (const auto& p : core::kParams)
        d.add(p.id, p.min, p.max, p.def, p.kind != core::ParamKind::Float);
    return d;
}

const Entry* ParamDocument::find(const std::string& id) const noexcept {
    const auto it = e_.find(id);
    return it == e_.end() ? nullptr : &it->second;
}

double ParamDocument::get(const std::string& id) const noexcept {
    const Entry* e = find(id);
    return e ? e->value : std::nan("");
}

bool ParamDocument::set(const std::string& id, double plain) {
    auto it = e_.find(id);
    if (it == e_.end() || !std::isfinite(plain)) return false;
    Entry& e = it->second;
    double v = std::fmin(e.max, std::fmax(e.min, plain));
    if (e.discrete) v = std::round(v);
    e.value = v;
    return true;
}

double ParamDocument::normalized(const std::string& id) const noexcept {
    const Entry* e = find(id);
    return e ? (e->value - e->min) / (e->max - e->min) : std::nan("");
}

std::string ParamDocument::serialize() const {
    std::string s;
    char buf[64];
    for (const auto& kv : e_) {
        std::snprintf(buf, sizeof buf, "=%.17g\n", kv.second.value);
        s += kv.first;
        s += buf;
    }
    return s;
}

bool ParamDocument::restore(const std::string& blob) {
    std::map<std::string, double> vals;
    std::size_t pos = 0;
    while (pos < blob.size()) {
        const std::size_t nl = blob.find('\n', pos);
        if (nl == std::string::npos) return false;
        const std::string line = blob.substr(pos, nl - pos);
        pos = nl + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || !find(line.substr(0, eq))) return false;
        char* end = nullptr;
        const double v = std::strtod(line.c_str() + eq + 1, &end);
        if (end == line.c_str() + eq + 1 || *end != '\0' || !std::isfinite(v)) return false;
        vals[line.substr(0, eq)] = v;
    }
    for (const auto& kv : vals) {
        Entry& e = e_[kv.first];
        e.value = std::fmin(e.max, std::fmax(e.min, kv.second));
    }
    return true;
}

}  // namespace cd::uimodel
