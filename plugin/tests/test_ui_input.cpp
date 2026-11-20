// Pure C++ input regressions; these do not need JUCE or a display server.
#include "ui/workbench/GestureMath.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
}
}

int main() {
    using cd::plugin::ui::gesture::parseQuantity;
    for (const auto& text : {"470u", "470 uF", "0.47mF", " +470 uF ", "0.00047 F", "4.7e-4 F"}) {
        const auto q = parseQuantity(text, "F", 1e-6);
        check(q.ok && std::abs(q.value - 470e-6) < 1e-15, text);
    }
    const auto bare = parseQuantity("470", "F", 1e-6);
    check(bare.ok && std::abs(bare.value - 470e-6) < 1e-15, "bare values use display units");
    const auto signedValue = parseQuantity("-0.25", "");
    check(signedValue.ok && signedValue.value == -0.25, "signed plain values");
    for (const auto& text : {"1 0", "1e 2", "1e308 M", "nan", "inf", "1e999", "1e-999", "0x10", "+", "+-1", "1,25", "470 xyz"})
        check(!parseQuantity(text, "").ok, text);
    check(!parseQuantity(std::string("12\0junk", 7), "").ok, "embedded NUL is rejected");
    check(!parseQuantity("1", "F", std::numeric_limits<double>::infinity()).ok, "non-finite default unit scale rejected");
    check(!parseQuantity("1", "F", 0).ok, "zero default unit scale rejected");
    using namespace cd::plugin::ui::gesture;
    check(parseCrossfade("25 % A / 75 % B") == 0.75, "A/B display parses the B fraction");
    check(parseCrossfade("100% A / 0% B") == 0.0, "A/B endpoint A");
    check(parseCrossfade("0% A / 100% B") == 1.0, "A/B endpoint B");
    check(parseCrossfade("2.5e-1") == 0.25, "A/B bare plain value");
    for (const auto& text : {"50% A / 60% B", "50% B / 50% A", "50 A / 50 B", "50% A", "50% A / 50% B junk",
                             "150% A / -50% B", "1 0% A / 90% B", "1e999% A / 0% B", "1.1", "-0.1"})
        check(!parseCrossfade(text), text);
    check(parseHarmonicTarget("Balanced") == 0.0, "balanced harmonic target");
    check(parseHarmonicTarget("Odd 50 %") == -0.5, "odd harmonic target stays negative");
    check(parseHarmonicTarget("Even 50 %") == 0.5, "even harmonic target stays positive");
    check(parseHarmonicTarget("Odd 100%") == -1.0, "odd harmonic endpoint");
    check(parseHarmonicTarget("Even 100%") == 1.0, "even harmonic endpoint");
    check(parseHarmonicTarget("-2.5e-1") == -0.25, "bare harmonic plain value");
    for (const auto& text : {"Balanced junk", "Odd 50", "Even 50", "Odd -50%", "Even 101%", "Odd 1 0%",
                             "Even 1e 2%", "Odd 50% junk", "Even 50% Odd", "-1.1", "1.1"})
        check(!parseHarmonicTarget(text), text);
    for (const auto& text : {"800", "800 Hz", "8e2 Hz", "0.8 kHz"})
        check(parseBoundedQuantity(text, "Hz", 100, 4000) == 800.0, text);
    for (const auto& text : {"800 V", "800 Hz junk", "99 Hz", "4001 Hz"})
        check(!parseBoundedQuantity(text, "Hz", 100, 4000), text);
    check(parsePercentage("45") == 0.45 && parsePercentage("45 %") == 0.45, "bare and labelled percentages");
    check(parseBoundedQuantity("8.5", "", 2, 30) == 8.5, "plain resonance Q");
    for (const auto& text : {"", "garbage", "1 0", "1e 2", "+-1", "nan", "inf", "1e999", "1e-999", "0x10", "0.5 junk"}) {
        check(!parseCrossfade(text), text);
        check(!parseHarmonicTarget(text), text);
        check(!parsePercentage(text), text);
        check(!parseBoundedQuantity(text, "Hz", 100, 4000), text);
    }
    if (!failures) std::puts("UI numeric-input regressions passed");
    return failures ? 1 : 0;
}
