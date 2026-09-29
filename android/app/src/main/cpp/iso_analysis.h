#pragma once

// Finds a sensor's native ISOs from a dark-frame ISO sweep (lens covered,
// fixed shutter). Pure C++ so it can be unit tested on a host
// (test/native/iso_analysis_test.cpp).
//
// For every ISO we have the dark noise sigma in raw units (fraction of the
// black..white range). Dividing by the ISO refers it back to the sensor input:
//   * analog gain (a real amplifier step) lowers input-referred noise;
//   * digital gain / attenuation (a multiply after the ADC) scales signal and
//     noise together, so input-referred noise stays flat;
//   * a dual/high-conversion-gain (HCG) switch shows as a sudden large drop
//     in input-referred noise between two adjacent ISOs.
// So: the base (lowest native) ISO is where a flat low-end run ends, the HCG
// ISO is the biggest sudden drop, and the digital region starts at the HAL's
// SENSOR_MAX_ANALOG_SENSITIVITY (not inferable from dark frames alone).

#include <vector>

namespace vesper {

struct IsoSample {
    int iso = 0;
    double sigma = 0; // dark noise, fraction of (white - black)
    double mean = 0;  // dark level above black, same units (sanity: lens covered)
};

struct IsoAnalysis {
    bool valid = false;
    const char* error = "";       // why not valid
    int baseIso = 0;               // lowest ISO with real (analog) gain
    int hcgIso = 0;                // high-conversion-gain switch (0 = not found)
    int digitalFromIso = 0;        // first ISO that is digital gain only (0 = none in range)
    std::vector<int> nativeIsos;   // baseIso, hcgIso (if any)
    std::vector<double> inputNoise; // per sample, relative to the first sample
};

IsoAnalysis analyzeIsoSweep(const std::vector<IsoSample>& samples, int maxAnalogIso);

// "Cleanest image" exposure: reach `target` (exposure time x ISO product) by
//  1. the base native ISO (or the lowest ISO when not analysed), exposing with
//     the shutter up to the 180-degree limit `maxShutterNs` (1/(2*fps));
//  2. still too dark: the dual-gain (HCG) native ISO, shutter again;
//  3. still too dark: raise ISO above that with the shutter at 180 degrees.
// Too bright even at the fastest shutter: ISO drops below base (extended low)
// only as a last resort.
struct ExposureChoice { double exposureNs = 0; double iso = 0; };
ExposureChoice solveCleanExposure(double target, double minShutterNs, double maxShutterNs, int minIso, int isoCap,
                                  int baseIso, int hcgIso);

} // namespace vesper
