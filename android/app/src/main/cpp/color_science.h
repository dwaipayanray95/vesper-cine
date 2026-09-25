#pragma once

// DNG-spec colour math for turning Camera2 sensor calibration into the two
// things the GPU pipeline needs per frame:
//   * wbGains      — per-channel camera-space multipliers (G = 1), applied
//                    in the unpack pass *before* highlight clipping;
//   * camToRec2020 — 3x3 (row-major) taking white-balanced camera RGB to
//                    scene-linear Rec.2020, with the log-exposure scale folded in.
//
// Pure C++ with no Android dependencies so it can be unit tested on a host
// (see test/native/color_science_test.cpp).
//
// References: Adobe DNG Specification 1.6, chapter 6 ("Mapping Camera Color
// Space to CIE XYZ Space"), and dng_color_spec.cpp in the DNG SDK.

#include <array>
#include <cstdint>

namespace vesper {

using Mat3 = std::array<float, 9>; // row-major: m[row * 3 + col]
using Vec3 = std::array<float, 3>;

Mat3 mat3Identity();
Mat3 mat3Mul(const Mat3& a, const Mat3& b);
Vec3 mat3MulVec(const Mat3& m, const Vec3& v);
bool mat3Inverse(const Mat3& m, Mat3& out);
Mat3 mat3Lerp(const Mat3& a, const Mat3& b, float t);
Mat3 mat3Diag(const Vec3& d);

struct Chromaticity { double x, y; };

// EXIF/DNG LightSource code -> CCT in Kelvin, using the Adobe DNG SDK's values.
float illuminantToKelvin(int32_t lightSourceCode);

// CIE 1931 xy of a blackbody at `kelvin`, shifted perpendicular to the
// Planckian locus by Adobe-style `tint` (positive = magenta, negative = green;
// one unit = Duv 1/3000, matching Lightroom/ACR/Resolve tint scales).
Chromaticity kelvinTintToXy(double kelvin, double tint);

// Inverse of kelvinTintToXy (Robertson-free: Newton search along the locus).
void xyToKelvinTint(Chromaticity xy, double& kelvin, double& tint);

struct DngCalibration {
    Mat3 colorMatrix1 = mat3Identity();       // ACAMERA_SENSOR_COLOR_TRANSFORM1 (XYZ -> camera)
    Mat3 colorMatrix2 = mat3Identity();
    Mat3 forwardMatrix1 = mat3Identity();     // ACAMERA_SENSOR_FORWARD_MATRIX1 (WB camera -> XYZ D50)
    Mat3 forwardMatrix2 = mat3Identity();
    Mat3 calibration1 = mat3Identity();       // ACAMERA_SENSOR_CALIBRATION_TRANSFORM1
    Mat3 calibration2 = mat3Identity();
    bool haveColorMatrix2 = false;
    bool haveForwardMatrix1 = false;
    bool haveForwardMatrix2 = false;
    float illuminant1Kelvin = 2850.0f;
    float illuminant2Kelvin = 6500.0f;
};

// Everything the shader needs for one white-balance state.
struct ColorState {
    Vec3 wbGains{1, 1, 1};           // 1 / cameraNeutral, normalised to G = 1
    Mat3 camToRec2020 = mat3Identity(); // includes exposure scale
    double kelvin = 5600;
    double tint = 0;
    Vec3 cameraNeutral{1, 1, 1};
};

class ColorPipeline {
public:
    void setCalibration(const DngCalibration& cal);
    bool dualIlluminant() const;

    // Linear value that sensor clip (WB'd green = 1.0) maps to in the log
    // encoder's scene-linear input. 18% grey then sits `log2(k / 0.18)` stops
    // below clip. See README "Exposure & Apple Log".
    void setClipLinear(float k) { clipLinear_ = k; }
    float clipLinear() const { return clipLinear_; }

    // Kelvin/tint dial: neutral = CM(T) * XYZ(xy(T, tint)).
    ColorState fromKelvinTint(double kelvin, double tint) const;

    // Tap-to-WB / AsShotNeutral: iterate xy <-> neutral per DNG spec.
    ColorState fromCameraNeutral(const Vec3& cameraNeutral) const;

private:
    Mat3 colorMatrixAt(double kelvin) const;   // CC * CM, mired-interpolated
    Mat3 forwardMatrixAt(double kelvin) const;
    double weight1At(double kelvin) const;     // DNG weight on illuminant 1
    ColorState finish(const Vec3& neutral, Chromaticity whiteXy) const;

    DngCalibration cal_;
    float clipLinear_ = 8.0f;
};

// Bradford-adapted CIE XYZ (D50) -> linear Rec.2020 (D65).
extern const Mat3 kXyzD50ToRec2020;

} // namespace vesper
