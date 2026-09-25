#include "color_science.h"

#include <algorithm>
#include <cmath>

namespace vesper {

// Bradford-adapted D50 -> D65, then XYZ -> Rec.2020 (ITU-R BT.2020 primaries).
// Verified with numpy: XYZ(D50) maps to (1, 1, 1) within 1e-4.
const Mat3 kXyzD50ToRec2020 = {
     1.6472754917f, -0.3936023227f, -0.2359807224f,
    -0.6826185276f,  1.6476180994f,  0.0128165404f,
     0.0296627114f, -0.0629166979f,  1.2533964909f,
};

namespace {

constexpr Chromaticity kD50xy{0.34567, 0.35850};

Vec3 xyToXyz(Chromaticity c) {
    return { static_cast<float>(c.x / c.y), 1.0f, static_cast<float>((1.0 - c.x - c.y) / c.y) };
}

Chromaticity xyzToXy(const Vec3& v) {
    double s = static_cast<double>(v[0]) + v[1] + v[2];
    if (std::fabs(s) < 1e-12) return kD50xy;
    return { v[0] / s, v[1] / s };
}

// CIE 1960 UCS <-> CIE 1931 xy.
void xyToUv(Chromaticity c, double& u, double& v) {
    double d = -2.0 * c.x + 12.0 * c.y + 3.0;
    u = 4.0 * c.x / d;
    v = 6.0 * c.y / d;
}

Chromaticity uvToXy(double u, double v) {
    double d = 2.0 * u - 8.0 * v + 4.0;
    return { 3.0 * u / d, 2.0 * v / d };
}

// Krystek (1985) rational approximation of the Planckian locus in CIE 1960 uv,
// valid 1000-15000K (max error ~1e-5 against CIE tables).
void planckianUv(double t, double& u, double& v) {
    double t2 = t * t;
    u = (0.860117757 + 1.54118254e-4 * t + 1.28641212e-7 * t2) /
        (1.0 + 8.42420235e-4 * t + 7.08145163e-7 * t2);
    v = (0.317398726 + 4.22806245e-5 * t + 4.20481691e-8 * t2) /
        (1.0 - 2.89741816e-5 * t + 1.61456053e-7 * t2);
}

// Unit normal to the locus at t, oriented towards green (increasing v).
void locusGreenNormal(double t, double& nu, double& nv) {
    double u0, v0, u1, v1;
    planckianUv(t - 1.0, u0, v0);
    planckianUv(t + 1.0, u1, v1);
    double du = u1 - u0, dv = v1 - v0;
    double len = std::sqrt(du * du + dv * dv);
    nu = -dv / len;
    nv = du / len;
    if (nv < 0) { nu = -nu; nv = -nv; }
}

constexpr double kMinKelvin = 1500.0;
constexpr double kMaxKelvin = 15000.0;
constexpr double kTintScale = 3000.0; // Duv per tint unit = 1/3000 (Adobe convention)

const Mat3 kBradford = {
     0.8951f,  0.2664f, -0.1614f,
    -0.7502f,  1.7135f,  0.0367f,
     0.0389f, -0.0685f,  1.0296f,
};

Mat3 bradfordAdapt(const Vec3& srcWhite, const Vec3& dstWhite) {
    Vec3 s = mat3MulVec(kBradford, srcWhite);
    Vec3 d = mat3MulVec(kBradford, dstWhite);
    Mat3 inv;
    mat3Inverse(kBradford, inv);
    return mat3Mul(inv, mat3Mul(mat3Diag({d[0] / s[0], d[1] / s[1], d[2] / s[2]}), kBradford));
}

} // namespace

Mat3 mat3Identity() { return {1, 0, 0, 0, 1, 0, 0, 0, 1}; }

Mat3 mat3Mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    return r;
}

Vec3 mat3MulVec(const Mat3& m, const Vec3& v) {
    return { m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
             m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
             m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
}

bool mat3Inverse(const Mat3& m, Mat3& out) {
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12) return false;
    double inv = 1.0 / det;
    out = { static_cast<float>(A * inv), static_cast<float>(-(b * i - c * h) * inv), static_cast<float>((b * f - c * e) * inv),
            static_cast<float>(B * inv), static_cast<float>((a * i - c * g) * inv), static_cast<float>(-(a * f - c * d) * inv),
            static_cast<float>(C * inv), static_cast<float>(-(a * h - b * g) * inv), static_cast<float>((a * e - b * d) * inv) };
    return true;
}

Mat3 mat3Lerp(const Mat3& a, const Mat3& b, float t) {
    Mat3 r{};
    for (int k = 0; k < 9; ++k) r[k] = a[k] + (b[k] - a[k]) * t;
    return r;
}

Mat3 mat3Diag(const Vec3& d) { return {d[0], 0, 0, 0, d[1], 0, 0, 0, d[2]}; }

float illuminantToKelvin(int32_t code) {
    switch (code) {
        case 1:  return 5500.0f; // Daylight
        case 2:  return 4150.0f; // Fluorescent
        case 3:  return 2850.0f; // Tungsten
        case 4:  return 5500.0f; // Flash
        case 9:  return 5500.0f; // Fine weather
        case 10: return 6500.0f; // Cloudy
        case 11: return 7500.0f; // Shade
        case 12: return 6400.0f; // Daylight fluorescent
        case 13: return 5050.0f; // Day white fluorescent
        case 14: return 4150.0f; // Cool white fluorescent
        case 15: return 3525.0f; // White fluorescent
        case 16: return 2925.0f; // Warm white fluorescent
        case 17: return 2850.0f; // Standard light A
        case 18: return 5500.0f; // Standard light B
        case 19: return 6500.0f; // Standard light C
        case 20: return 5500.0f; // D55
        case 21: return 6500.0f; // D65
        case 22: return 7500.0f; // D75
        case 23: return 5000.0f; // D50
        case 24: return 3200.0f; // ISO studio tungsten
        default: return 5000.0f;
    }
}

Chromaticity kelvinTintToXy(double kelvin, double tint) {
    double t = std::clamp(kelvin, kMinKelvin, kMaxKelvin);
    double u, v, nu, nv;
    planckianUv(t, u, v);
    locusGreenNormal(t, nu, nv);
    double duv = -tint / kTintScale; // +tint = magenta = below the locus
    return uvToXy(u + duv * nu, v + duv * nv);
}

void xyToKelvinTint(Chromaticity xy, double& kelvin, double& tint) {
    double u, v;
    xyToUv(xy, u, v);
    // Distance to the locus is unimodal in mired over the valid range, so a
    // golden-section search converges reliably.
    auto dist2 = [&](double mired) {
        double lu, lv;
        planckianUv(1.0e6 / mired, lu, lv);
        return (u - lu) * (u - lu) + (v - lv) * (v - lv);
    };
    double lo = 1.0e6 / kMaxKelvin, hi = 1.0e6 / kMinKelvin;
    const double phi = 0.6180339887498949;
    double a = hi - phi * (hi - lo), b = lo + phi * (hi - lo);
    double fa = dist2(a), fb = dist2(b);
    for (int it = 0; it < 80; ++it) {
        if (fa < fb) { hi = b; b = a; fb = fa; a = hi - phi * (hi - lo); fa = dist2(a); }
        else         { lo = a; a = b; fa = fb; b = lo + phi * (hi - lo); fb = dist2(b); }
    }
    double t = 1.0e6 / (0.5 * (lo + hi));
    double lu, lv, nu, nv;
    planckianUv(t, lu, lv);
    locusGreenNormal(t, nu, nv);
    double duv = (u - lu) * nu + (v - lv) * nv;
    kelvin = t;
    tint = -duv * kTintScale;
}

void ColorPipeline::setCalibration(const DngCalibration& cal) {
    cal_ = cal;
    // Keep illuminant 1 as the warmer one so weight1At() stays monotonic.
    if (cal_.haveColorMatrix2 && cal_.illuminant1Kelvin > cal_.illuminant2Kelvin) {
        std::swap(cal_.colorMatrix1, cal_.colorMatrix2);
        std::swap(cal_.forwardMatrix1, cal_.forwardMatrix2);
        std::swap(cal_.calibration1, cal_.calibration2);
        std::swap(cal_.illuminant1Kelvin, cal_.illuminant2Kelvin);
        if (cal_.haveForwardMatrix1 != cal_.haveForwardMatrix2) {
            // Only one forward matrix: it belongs to whichever slot it came from.
            std::swap(cal_.haveForwardMatrix1, cal_.haveForwardMatrix2);
        }
    }
    if (std::fabs(cal_.illuminant1Kelvin - cal_.illuminant2Kelvin) < 50.0f) {
        cal_.haveColorMatrix2 = false; // degenerate pair: treat as single illuminant
    }
}

bool ColorPipeline::dualIlluminant() const { return cal_.haveColorMatrix2; }

double ColorPipeline::weight1At(double kelvin) const {
    if (!cal_.haveColorMatrix2) return 1.0;
    double t1 = cal_.illuminant1Kelvin, t2 = cal_.illuminant2Kelvin;
    if (kelvin <= t1) return 1.0;
    if (kelvin >= t2) return 0.0;
    return (1.0 / kelvin - 1.0 / t2) / (1.0 / t1 - 1.0 / t2);
}

Mat3 ColorPipeline::colorMatrixAt(double kelvin) const {
    float w = static_cast<float>(weight1At(kelvin));
    Mat3 cm = mat3Lerp(cal_.colorMatrix2, cal_.colorMatrix1, w);
    Mat3 cc = mat3Lerp(cal_.calibration2, cal_.calibration1, w);
    if (!cal_.haveColorMatrix2) { cm = cal_.colorMatrix1; cc = cal_.calibration1; }
    return mat3Mul(cc, cm);
}

Mat3 ColorPipeline::forwardMatrixAt(double kelvin) const {
    if (cal_.haveForwardMatrix1 && cal_.haveForwardMatrix2 && cal_.haveColorMatrix2) {
        return mat3Lerp(cal_.forwardMatrix2, cal_.forwardMatrix1, static_cast<float>(weight1At(kelvin)));
    }
    return cal_.haveForwardMatrix1 ? cal_.forwardMatrix1 : cal_.forwardMatrix2;
}

ColorState ColorPipeline::finish(const Vec3& neutralIn, Chromaticity whiteXy) const {
    ColorState s;
    Vec3 n = neutralIn;
    float g = std::max(n[1], 1e-6f);
    for (auto& c : n) c = std::max(c / g, 1e-4f);
    s.cameraNeutral = n;
    s.wbGains = {1.0f / n[0], 1.0f, 1.0f / n[2]};
    xyToKelvinTint(whiteXy, s.kelvin, s.tint);

    float w = static_cast<float>(weight1At(s.kelvin));
    Mat3 cc = cal_.haveColorMatrix2 ? mat3Lerp(cal_.calibration2, cal_.calibration1, w) : cal_.calibration1;
    Mat3 ccInv;
    if (!mat3Inverse(cc, ccInv)) ccInv = mat3Identity();

    Mat3 camToXyzD50;
    if (cal_.haveForwardMatrix1 || cal_.haveForwardMatrix2) {
        // DNG 1.6 §6.3: CameraToXYZ_D50 = FM * D * Inverse(AB * CC), D = diag(1 / ReferenceNeutral).
        Vec3 ref = mat3MulVec(ccInv, n);
        Mat3 d = mat3Diag({1.0f / std::max(ref[0], 1e-6f), 1.0f / std::max(ref[1], 1e-6f), 1.0f / std::max(ref[2], 1e-6f)});
        camToXyzD50 = mat3Mul(forwardMatrixAt(s.kelvin), mat3Mul(d, ccInv));
    } else {
        // No forward matrix: invert the colour matrix, scale so the neutral has
        // Y = 1, then Bradford-adapt from the scene white to D50.
        Mat3 cmInv;
        if (!mat3Inverse(colorMatrixAt(s.kelvin), cmInv)) cmInv = mat3Identity();
        Vec3 xyzN = mat3MulVec(cmInv, n);
        float scale = 1.0f / std::max(xyzN[1], 1e-6f);
        for (auto& e : cmInv) e *= scale;
        camToXyzD50 = mat3Mul(bradfordAdapt(xyToXyz(whiteXy), xyToXyz(kD50xy)), cmInv);
    }

    // The shader multiplies by wbGains first, so fold diag(neutral) back in:
    // camToRec2020 * (raw * wbGains) == k * XYZtoRec2020 * CameraToXYZ * raw.
    Mat3 m = mat3Mul(kXyzD50ToRec2020, mat3Mul(camToXyzD50, mat3Diag(n)));
    for (auto& e : m) e *= clipLinear_;
    s.camToRec2020 = m;
    return s;
}

ColorState ColorPipeline::fromKelvinTint(double kelvin, double tint) const {
    Chromaticity xy = kelvinTintToXy(kelvin, tint);
    Vec3 neutral = mat3MulVec(colorMatrixAt(kelvin), xyToXyz(xy));
    ColorState s = finish(neutral, xy);
    s.kelvin = kelvin; // report the dial value, not the round-tripped estimate
    s.tint = tint;
    return s;
}

ColorState ColorPipeline::fromCameraNeutral(const Vec3& neutral) const {
    // DNG 1.6 §6.2.1: find the white xy such that CM(xy) * XYZ(xy) ∝ neutral.
    Chromaticity xy = kD50xy;
    for (int it = 0; it < 30; ++it) {
        double k, t;
        xyToKelvinTint(xy, k, t);
        Mat3 cmInv;
        if (!mat3Inverse(colorMatrixAt(k), cmInv)) break;
        Chromaticity next = xyzToXy(mat3MulVec(cmInv, neutral));
        if (!(next.y > 0.01)) break; // guard against garbage neutrals
        bool converged = std::fabs(next.x - xy.x) + std::fabs(next.y - xy.y) < 1e-7;
        xy = next;
        if (converged) break;
    }
    return finish(neutral, xy);
}

} // namespace vesper
