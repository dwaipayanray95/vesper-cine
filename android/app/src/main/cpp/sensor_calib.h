#pragma once

// Sensor calibration statistics for tools/calibration/sensor.py.
//
// The Dark / White sweeps (Settings > Developer) capture a burst of identical
// RAW10 frames at each ISO / shutter step. Uploading every frame would be
// gigabytes, so the phone reduces each burst to what the analysis needs:
//   * per 128x128-px block and CFA site: mean level, temporal noise (from
//     frame-pair differences, so the scene, lens shading and fixed-pattern
//     noise cancel; a block-level offset such as light flicker is removed too),
//     spatial variance of the burst mean, clipped-sample count;
//   * per site: row / column noise (banding), a histogram of the top 128 DN
//     codes and the highest DN seen (sensor clip point);
//   * defective photosites: burst mean vs the median of its 8 same-colour
//     neighbours, more than 6 sigma (single-frame temporal noise) and 4 DN off.
// Pure C++ (no Android) so it is unit tested on a host:
// test/native/sensor_calib_test.cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace vesper {

struct SensorStepInput {
    int width = 0, height = 0, rowStride = 0;
    int cfa = 0;                            // 0=RGGB 1=GRBG 2=GBRG 3=BGGR
    float black[4] = {64, 64, 64, 64};     // logical R, Gr, Gb, B (DN)
    float white = 1023.0f;
    std::vector<const uint8_t*> frames;    // >= 2 RAW10 frames, same settings
    int block = 128;                       // raw px, multiple of 4
    bool findDefects = true;
    int maxDefects = 4000;                 // largest |excess| kept; defectCount counts all
};

struct SensorDefect {
    int x = 0, y = 0, site = 0;            // raw px, logical site
    float value = 0;                       // burst mean, DN (incl. black)
    float excess = 0;                      // value - median of the 8 same-colour neighbours, DN
    float sigma = 0;                       // single-frame temporal noise of its block, DN
};

struct SensorStepStats {
    bool valid = false;
    int frames = 0, pairs = 0;
    int block = 0, blocksX = 0, blocksY = 0, x0 = 0, y0 = 0; // grid origin, raw px
    std::vector<float> mean[4];            // per block, row-major; DN incl. black
    std::vector<float> tvar[4];            // single-frame temporal variance, DN^2 (outliers > 6 sigma dropped)
    std::vector<float> svar[4];            // spatial variance of the burst mean in the block, DN^2
    std::vector<int> clipped[4];           // samples >= white - 1, all frames
    double siteMean[4] = {0, 0, 0, 0};     // over the grid, DN
    double siteTvar[4] = {0, 0, 0, 0};     // mean of the block tvars, DN^2
    double rowVar[4] = {0, 0, 0, 0};       // variance of a row's mean, single frame, DN^2 (adjacent-row differences: smooth flicker cancels)
    double colVar[4] = {0, 0, 0, 0};
    int rowSamples[4] = {0, 0, 0, 0};      // samples averaged per row / column mean
    int colSamples[4] = {0, 0, 0, 0};
    int maxDn[4] = {0, 0, 0, 0};
    std::vector<uint32_t> topHist[4];      // counts of DN 896..1023, all frames
    std::vector<SensorDefect> defects;     // largest |excess| first
    int defectCount = 0, hotCount = 0, deadCount = 0;
};

SensorStepStats analyzeSensorStep(const SensorStepInput& in);

// JSON object (no surrounding whitespace) with every field of `s`.
std::string sensorStepJson(const SensorStepStats& s);

// Mean of a centred patch (`fraction` of width and height) of the chosen
// sites (bit mask over logical R, Gr, Gb, B), normalised (DN - black) /
// (white - black). Used to meter the White sweep.
double rawPatchMean(const uint8_t* data, int width, int height, int rowStride, int cfa, const float black[4],
                    float white, double fraction, int siteMask);

// Static defect (hot-pixel) map, applied before the GPU sees a frame: every
// listed raw pixel (x, y pairs, raw-stream coordinates) is replaced by the
// median of its 8 same-colour neighbours (2 px apart), read from `src`. The
// GPU's upload buffer `dst` is write-combined (slow to read), so the repaired
// 5-byte RAW10 groups are built from `src` and only those are written.
// `xy` must be sorted by (y, x / 4) so pixels of one group are applied together.
void repairRawDefects(const uint8_t* src, uint8_t* dst, int width, int height, int rowStride, const int32_t* xy,
                      size_t count);

// Sorts defect pixels for repairRawDefects and drops those within 2 px of the
// frame edge (no full neighbourhood).
std::vector<int32_t> prepareDefects(std::vector<int32_t> xy, int width, int height);

// Measured / HAL dark-noise ratio for this ISO from a calibration table
// (log-log interpolation, held flat beyond the ends). 1 without a table.
double noiseFactorAt(const std::vector<float>& isos, const std::vector<float>& factors, double iso);

} // namespace vesper
