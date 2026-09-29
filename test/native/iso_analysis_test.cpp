// Host test for android/app/src/main/cpp/iso_analysis.cpp: synthetic sensors
// with known gain structure.
#include "iso_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace vesper;
static int failures = 0;
static void check(bool ok, const char* what, int got, int want) {
    if (!ok) { std::printf("FAIL %s (got %d, want %d)\n", what, got, want); ++failures; }
}

// Dark noise in raw units for a sensor with: digital attenuation below
// `base`, analog gain from base, a conversion-gain switch at `hcg` that cuts
// pre-amp read noise to `hcgFactor`, and digital gain above `analogMax`.
static std::vector<IsoSample> sweep(int base, int hcg, double hcgFactor, int analogMax, int maxIso) {
    std::vector<IsoSample> out;
    const double pre = 2.0, post = 6.0; // electrons-ish, pre- and post-amplifier read noise
    for (double iso = 32; iso <= maxIso * 1.001; iso *= std::cbrt(2.0)) {
        int i = static_cast<int>(std::lround(iso));
        double analogGain = std::clamp(iso, (double)base, (double)analogMax) / base;
        double digital = iso / (analogGain * base);
        double preNoise = (hcg > 0 && iso >= hcg) ? pre * hcgFactor : pre;
        double postNoise = (hcg > 0 && iso >= hcg) ? post * hcgFactor : post;
        double dn = std::sqrt(std::pow(preNoise * analogGain, 2) + postNoise * postNoise) * digital;
        out.push_back({i, dn / 4000.0, 0.001});
    }
    return out;
}

int main() {
    // Dual gain: base 50, HCG at ~200, digital above 3200 (reported by the HAL).
    auto a = analyzeIsoSweep(sweep(50, 203, 0.4, 3200, 12800), 3200);
    check(a.valid, "dual-gain analysis valid", a.valid, 1);
    check(std::abs(a.baseIso - 50) <= 1, "base ISO found below the extended low range", a.baseIso, 50);
    check(std::abs(a.hcgIso - 203) <= 3, "HCG switch found", a.hcgIso, 203);
    check(a.digitalFromIso > 3200 && a.digitalFromIso < 4100, "digital gain above analog max", a.digitalFromIso, 4032);

    // Same sensor, HAL doesn't report the analog limit: natives still found,
    // digital region reported as unknown rather than guessed.
    auto b = analyzeIsoSweep(sweep(50, 203, 0.4, 3200, 12800), 0);
    check(b.valid && std::abs(b.hcgIso - 203) <= 3, "HCG found without HAL analog limit", b.hcgIso, 203);
    check(b.digitalFromIso == 0, "digital region not guessed", b.digitalFromIso, 0);

    // Single gain sensor: no HCG reported.
    auto c = analyzeIsoSweep(sweep(64, 0, 1.0, 6400, 6400), 6400);
    check(c.valid && c.hcgIso == 0, "no HCG on a single-gain sensor", c.hcgIso, 0);
    check(std::abs(c.baseIso - 64) <= 1, "single-gain base ISO", c.baseIso, 64);

    // Lens not covered.
    auto d = sweep(50, 203, 0.4, 3200, 3200);
    for (auto& x : d) x.mean = 0.2;
    check(!analyzeIsoSweep(d, 3200).valid, "rejects a lit frame", 0, 0);

    // Clean exposure solver: 24 fps -> 180 deg = 20.8 ms; base 50, HCG 200, cap 3200.
    {
        const double minT = 20e3, maxT = 1e9 / 48;
        auto e = [&](double target) { return solveCleanExposure(target, minT, maxT, 32, 3200, 50, 200); };
        auto a1 = e(50 * 5e6);   // bright: base ISO, 1/200
        check(a1.iso == 50 && std::fabs(a1.exposureNs - 5e6) < 1, "bright scene: base ISO, shutter exposes", (int)a1.iso, 50);
        auto a2 = e(50 * 30e6);  // needs more than 180 deg at base -> HCG native
        check(a2.iso == 200 && a2.exposureNs <= maxT, "dim: jumps to HCG native before gaining up", (int)a2.iso, 200);
        auto a3 = e(800 * maxT); // darker than HCG at 180 deg -> gain up, shutter stays at 180 deg
        check(std::fabs(a3.iso - 800) < 1 && std::fabs(a3.exposureNs - maxT) < 1, "dark: shutter held at 180, ISO rises", (int)a3.iso, 800);
        auto a4 = e(40 * minT);  // brighter than base at fastest shutter -> extended low
        check(a4.iso < 50 && a4.iso >= 32 && a4.exposureNs == minT, "very bright: extended-low ISO as last resort", (int)a4.iso, 40);
        auto a5 = solveCleanExposure(100 * 30e6, minT, maxT, 32, 3200, 0, 0); // not analysed: lowest ISO first
        check(a5.exposureNs == maxT && a5.iso > 32, "uncalibrated: lowest ISO, shutter first, then gain", (int)a5.iso, 144);
    }

    std::printf(failures ? "%d ISO ANALYSIS FAILURES\n" : "all ISO analysis tests passed\n", failures);
    return failures ? 1 : 0;
}
