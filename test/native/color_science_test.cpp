// Host unit test for android/app/src/main/cpp/color_science.cpp.
// Build & run:  test/native/run_tests.sh
#include "color_science.h"

#include <cmath>
#include <cstdio>

using namespace vesper;

static int failures = 0;
#define CHECK_NEAR(a, b, tol, what)                                                   \
    do {                                                                              \
        double _a = (a), _b = (b);                                                    \
        if (std::fabs(_a - _b) > (tol)) {                                             \
            std::printf("FAIL %s: %.6f vs %.6f\n", what, _a, _b);                     \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

// A plausible phone-sensor calibration (IMX-class values, as DngCreator emits).
static DngCalibration sampleCalibration() {
    DngCalibration c;
    c.colorMatrix1 = {0.9563f, -0.5093f, 0.0391f, -0.4212f, 1.2135f, 0.2279f, -0.0572f, 0.1621f, 0.6307f};
    c.colorMatrix2 = {0.7497f, -0.2479f, -0.0690f, -0.4382f, 1.2295f, 0.2091f, -0.0602f, 0.1398f, 0.5715f};
    c.forwardMatrix1 = {0.6216f, 0.2254f, 0.1173f, 0.2129f, 0.8401f, -0.0530f, 0.0129f, -0.2426f, 1.0548f};
    c.forwardMatrix2 = {0.6523f, 0.2604f, 0.0516f, 0.2689f, 0.9186f, -0.1875f, 0.0217f, -0.1476f, 0.9510f};
    c.haveColorMatrix2 = c.haveForwardMatrix1 = c.haveForwardMatrix2 = true;
    c.illuminant1Kelvin = illuminantToKelvin(17);
    c.illuminant2Kelvin = illuminantToKelvin(21);
    return c;
}

int main() {
    // D50 -> Rec.2020 must map D50 white to equal-energy RGB.
    Vec3 d50 = {0.9642f, 1.0f, 0.8249f};
    Vec3 w = mat3MulVec(kXyzD50ToRec2020, d50);
    for (int i = 0; i < 3; ++i) CHECK_NEAR(w[i], 1.0, 2e-3, "D50 -> Rec2020 white");

    // Kelvin/tint <-> xy round trip.
    for (double k : {2500.0, 3200.0, 5600.0, 6500.0, 9000.0}) {
        for (double t : {-40.0, 0.0, 35.0}) {
            double k2, t2;
            xyToKelvinTint(kelvinTintToXy(k, t), k2, t2);
            CHECK_NEAR(k2, k, k * 2e-3, "kelvin round trip");
            CHECK_NEAR(t2, t, 0.2, "tint round trip");
        }
    }
    // Positive tint must move towards magenta (lower y than the locus).
    if (!(kelvinTintToXy(5600, 30).y < kelvinTintToXy(5600, 0).y)) { std::puts("FAIL tint sign"); ++failures; }

    ColorPipeline p;
    p.setCalibration(sampleCalibration());
    p.setClipLinear(1.0f);

    // Kelvin dial: a grey lit by that illuminant (raw == neutral) must come
    // out neutral with Y == 1 at G clip.
    for (double k : {2850.0, 4000.0, 5600.0, 7500.0}) {
        ColorState s = p.fromKelvinTint(k, 0);
        Vec3 wb = {s.cameraNeutral[0] * s.wbGains[0], s.cameraNeutral[1] * s.wbGains[1], s.cameraNeutral[2] * s.wbGains[2]};
        Vec3 rgb = mat3MulVec(s.camToRec2020, wb);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(rgb[i], 1.0, 3e-3, "kelvin neutral renders grey");
        CHECK_NEAR(s.wbGains[1], 1.0, 1e-6, "green gain is 1");
    }
    // Warmer light -> more red in the raw neutral -> smaller red gain.
    if (!(p.fromKelvinTint(3000, 0).wbGains[0] < p.fromKelvinTint(6500, 0).wbGains[0])) {
        std::puts("FAIL red gain should shrink as light warms"); ++failures;
    }

    // Tap-to-WB: neutral from the dial at 4300K must invert back to ~4300K.
    ColorState dial = p.fromKelvinTint(4300, 12);
    ColorState tap = p.fromCameraNeutral(dial.cameraNeutral);
    CHECK_NEAR(tap.kelvin, 4300, 20, "neutral -> kelvin");
    CHECK_NEAR(tap.tint, 12, 1.0, "neutral -> tint");
    Vec3 rgb = mat3MulVec(tap.camToRec2020, {1, 1, 1});
    for (int i = 0; i < 3; ++i) CHECK_NEAR(rgb[i], 1.0, 3e-3, "tap neutral renders grey");

    // Exposure scale is linear in clipLinear.
    p.setClipLinear(8.0f);
    Vec3 rgb8 = mat3MulVec(p.fromKelvinTint(5600, 0).camToRec2020, {1, 1, 1});
    CHECK_NEAR(rgb8[1], 8.0, 0.03, "clipLinear scales output");

    // Single-illuminant device without forward matrices still renders grey.
    DngCalibration single = sampleCalibration();
    single.haveColorMatrix2 = single.haveForwardMatrix1 = single.haveForwardMatrix2 = false;
    ColorPipeline ps;
    ps.setCalibration(single);
    ps.setClipLinear(1.0f);
    ColorState ss = ps.fromKelvinTint(5000, 0);
    Vec3 g = mat3MulVec(ss.camToRec2020, {1, 1, 1});
    for (int i = 0; i < 3; ++i) CHECK_NEAR(g[i], 1.0, 5e-3, "no-FM neutral renders grey");

    std::printf(failures ? "%d FAILURES\n" : "all color_science tests passed\n", failures);
    return failures ? 1 : 0;
}
