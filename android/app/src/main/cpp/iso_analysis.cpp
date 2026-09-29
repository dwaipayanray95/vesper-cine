#include "iso_analysis.h"

#include <cmath>

namespace vesper {

namespace {
constexpr double kHcgDrop = 0.72;   // input noise falls >28% in one step: a gain-mode switch
constexpr double kFlat = 0.06;      // |ratio - 1| below this: digital (noise scales with signal)
constexpr double kMaxDarkMean = 0.02; // above 2% of range the lens isn't covered
}

IsoAnalysis analyzeIsoSweep(const std::vector<IsoSample>& s, int maxAnalogIso) {
    IsoAnalysis a;
    if (s.size() < 4) { a.error = "too few ISO steps measured"; return a; }
    for (const auto& x : s) {
        if (x.iso <= 0 || !(x.sigma > 0)) { a.error = "invalid noise measurement"; return a; }
        if (x.mean > kMaxDarkMean) { a.error = "frame not dark: cover the lens completely"; return a; }
    }
    // Input-referred noise (relative): sigma / gain, gain proportional to ISO.
    const double n0 = s[0].sigma / s[0].iso;
    for (const auto& x : s) a.inputNoise.push_back((x.sigma / x.iso) / n0);

    // Low-end digital attenuation: flat input noise from the bottom up.
    size_t base = 0;
    while (base + 1 < s.size() && std::fabs(a.inputNoise[base + 1] / a.inputNoise[base] - 1) < kFlat) ++base;
    if (base + 1 >= s.size()) { a.error = "noise never changes with ISO"; return a; }
    a.baseIso = s[base].iso;

    // Digital region at the top: only from the HAL's analog limit. In a dark
    // frame, high analog gain (pre-amp noise dominated) and digital gain both
    // leave input noise flat, so it can't be inferred reliably.
    size_t top = s.size();
    if (maxAnalogIso > 0) {
        for (size_t i = 0; i < s.size(); ++i)
            if (s[i].iso > maxAnalogIso) { top = i; break; }
    }
    a.digitalFromIso = top < s.size() ? s[top].iso : 0;

    // HCG switch: the biggest sudden drop inside the analog range.
    double best = kHcgDrop;
    for (size_t i = base + 1; i < top; ++i) {
        double r = a.inputNoise[i] / a.inputNoise[i - 1];
        if (r < best) { best = r; a.hcgIso = s[i].iso; }
    }
    a.nativeIsos.push_back(a.baseIso);
    if (a.hcgIso > 0) a.nativeIsos.push_back(a.hcgIso);
    a.valid = true;
    return a;
}

} // namespace vesper
