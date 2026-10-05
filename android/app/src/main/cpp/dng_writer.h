#pragma once

// Minimal DNG 1.4 writer for Calibration Frame captures: the RAW10 frame
// unpacked to 16-bit, uncompressed, with everything a raw developer (or
// tools/calibration) needs from the CaptureResult: black / white level per
// CFA site, the Camera2 colour matrices, the HAL's AWB neutral (AsShotNeutral),
// the noise profile, exposure, and the lens-shading map as GainMap opcodes
// (OpcodeList2) so Lightroom / RawTherapee correct vignetting like the app.
// Pure C++ so it is unit tested on a host (test/native/sensor_calib_test.cpp).

#include <array>
#include <cstdint>
#include <string>

namespace vesper {

struct DngImageInfo {
    int width = 0, height = 0, rowStride = 0;
    int cfa = 0;                                   // 0=RGGB 1=GRBG 2=GBRG 3=BGGR
    float black[4] = {64, 64, 64, 64};             // logical R, Gr, Gb, B (DN)
    float white = 1023.0f;
    std::array<float, 9> colorMatrix1{1, 0, 0, 0, 1, 0, 0, 0, 1}, colorMatrix2{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 9> calibration1{1, 0, 0, 0, 1, 0, 0, 0, 1}, calibration2{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 9> forwardMatrix1{1, 0, 0, 0, 1, 0, 0, 0, 1}, forwardMatrix2{1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool haveColorMatrix2 = false, haveForwardMatrix1 = false, haveForwardMatrix2 = false;
    int illuminant1 = 17, illuminant2 = 21;        // EXIF LightSource codes (17 = std A, 21 = D65)
    float asShotNeutral[3] = {1, 1, 1};            // camera RGB of a neutral
    double noiseProfile[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // SENSOR_NOISE_PROFILE (S, O) pairs, CFA 2x2 row-major
    int noiseChannels = 0;
    int64_t exposureNs = 0;
    int iso = 0;
    float fNumber = 0, focalLength = 0;            // 0 = unknown
    int orientation = 1;                           // TIFF: 1 normal, 6 = 90 cw, 3 = 180, 8 = 270 cw
    std::string make = "Google", model, uniqueModel, software = "Vesper Cine", dateTime; // "YYYY:MM:DD HH:MM:SS"
    const float* shading = nullptr;                // [row][col][R, Geven, Godd, B] over the pre-correction array
    int shadingCols = 0, shadingRows = 0;
    float sensorMap[4] = {1, 1, 0, 0};             // raw px -> array px: array = raw * xy + zw
    float arrayWidth = 0, arrayHeight = 0;
};

// Writes `raw10` (rowStride bytes per row) as a DNG. false + *error on failure.
bool writeDng(const std::string& path, const uint8_t* raw10, const DngImageInfo& info, std::string* error = nullptr);

} // namespace vesper
