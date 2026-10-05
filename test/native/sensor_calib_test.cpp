// Host test for sensor_calib.cpp (burst statistics) and dng_writer.cpp.
// A synthetic sensor with known noise, black level, clip point, vignetting,
// defects, row noise and light flicker: the statistics must recover them.
// With an output directory argument it also writes a synthetic Dark and White
// sweep (VSENSOR_*_synthetic.json, same layout as the phone's) for
// tools/calibration/test_sensor.py, with the truth under "synthTruth".
#include "dng_writer.h"
#include "sensor_calib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace vesper;
static int failures = 0;
static void check(bool ok, const char* what, double got, double want) {
    if (!ok) {
        std::printf("FAIL %s (got %.4g, want %.4g)\n", what, got, want);
        ++failures;
    }
}

static int siteOf(int cfa, int px, int py) {
    int bit = ((py & 1) << 1) | (px & 1);
    switch (cfa) {
        case 0: return bit;
        case 1: return bit ^ 1;
        case 2: return bit ^ 2;
        default: return 3 - bit;
    }
}

static std::vector<uint8_t> pack(const std::vector<uint16_t>& dn, int w, int h, int stride) {
    std::vector<uint8_t> out(static_cast<size_t>(stride) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int g = 0; g < w / 4; ++g) {
            uint8_t* p = &out[static_cast<size_t>(y) * stride + g * 5];
            p[4] = 0;
            for (int i = 0; i < 4; ++i) {
                uint16_t v = dn[static_cast<size_t>(y) * w + g * 4 + i];
                p[i] = static_cast<uint8_t>(v >> 2);
                p[4] |= static_cast<uint8_t>((v & 3) << (2 * i));
            }
        }
    return out;
}

// --- The synthetic sensor -----------------------------------------------------
struct Sensor {
    int w = 256, h = 192, cfa = 1;              // GRBG
    double blackTrue[4] = {64.0, 64.6, 64.6, 63.4};
    double blackReported = 64.0;
    double satTrue = 1008;                      // real clip, reported white 1023
    double K50 = 0.30, readPre = 1.2, readPost = 1.0; // DN/e-like gain at ISO 50, read noise (DN at ISO 50)
    double rowSigma50 = 0.15;                   // row noise, DN at ISO 50 (scales with gain)
    double vig[4] = {1.6, 1.2, 1.2, 1.0};       // vignetting strength per site: V = 1 / (1 + k r^2)
    double mapError = 0.06;                     // HAL map under-corrects R by 6% at the corners
    double flicker = 0.01;                      // per-frame light level change (fraction)
    struct Px { int x, y; double hot50; bool dead; };
    std::vector<Px> defects = {{40, 30, 40, false}, {101, 77, 20, false}, {200, 150, 100, false}, {150, 40, 0, true}};

    double gainFactor(int iso) const { return iso / 50.0; }
    double K(int iso) const { return K50 * gainFactor(iso); }
    double read(int iso) const { return std::hypot(readPre * gainFactor(iso), readPost); }
    double r2(int x, int y) const {
        double dx = (x + 0.5 - w / 2.0) / (w / 2.0), dy = (y + 0.5 - h / 2.0) / (w / 2.0);
        return dx * dx + dy * dy;
    }
    double V(int site, int x, int y) const { return 1.0 / (1.0 + vig[site] * r2(x, y)); }
    // HAL map value (gain) at array position; channel 0=R 1=Geven 2=Godd 3=B.
    double mapGain(int channel, double ax, double ay) const {
        int site = channel == 0 ? 0 : channel == 3 ? 3 : 1;
        double dx = (ax - w / 2.0) / (w / 2.0), dy = (ay - h / 2.0) / (w / 2.0);
        double rr = dx * dx + dy * dy;
        double g = 1.0 + vig[site] * rr;
        if (channel == 0) g *= 1.0 - mapError * rr / 1.5625; // 1.5625 = corner r^2 for 4:3
        return g;
    }

    // `flux`: DN per ns at ISO 50 in the centre for green (0 = dark).
    std::vector<std::vector<uint8_t>> burst(int iso, double exposureNs, double flux, int n, std::mt19937& rng) const {
        std::normal_distribution<double> N01(0.0, 1.0);
        std::vector<std::vector<uint8_t>> out;
        const int stride = w * 5 / 4 + 8;
        for (int f = 0; f < n; ++f) {
            double fl = 1.0 + flicker * N01(rng);
            std::vector<uint16_t> dn(static_cast<size_t>(w) * h);
            for (int y = 0; y < h; ++y) {
                double row = rowSigma50 * gainFactor(iso) * N01(rng);
                for (int x = 0; x < w; ++x) {
                    int site = siteOf(cfa, x, y);
                    double colour = site == 0 ? 0.55 : site == 3 ? 0.7 : 1.0;
                    double sig = flux * exposureNs * gainFactor(iso) * colour * V(site, x, y) * fl;
                    for (const auto& d : defects)
                        if (d.x == x && d.y == y) sig = d.dead ? sig * 0.2 : sig + d.hot50 * gainFactor(iso) * exposureNs / 30e6;
                    double v = blackTrue[site] + sig + row + std::sqrt(K(iso) * sig + read(iso) * read(iso)) * N01(rng);
                    v = std::clamp(std::round(v), 0.0, satTrue);
                    dn[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(v);
                }
            }
            out.push_back(pack(dn, w, h, stride));
        }
        return out;
    }
    int stride() const { return w * 5 / 4 + 8; }
};

static SensorStepStats analyze(const Sensor& s, const std::vector<std::vector<uint8_t>>& frames, int block = 32) {
    SensorStepInput in;
    in.width = s.w;
    in.height = s.h;
    in.rowStride = s.stride();
    in.cfa = s.cfa;
    for (float& b : in.black) b = static_cast<float>(s.blackReported);
    in.white = 1023;
    in.block = block;
    for (const auto& f : frames) in.frames.push_back(f.data());
    return analyzeSensorStep(in);
}

static void testStatistics() {
    std::mt19937 rng(7);
    Sensor s;
    s.vig[0] = s.vig[1] = s.vig[2] = s.vig[3] = 0; // flat for the noise checks
    s.defects.clear();
    s.flicker = 0.02;
    const int iso = 400;
    const double flux = 400.0 / 8.0 / 30e6; // ~400 DN green at ISO 400, 30 ms
    auto fr = s.burst(iso, 30e6, flux, 4, rng);
    SensorStepStats st = analyze(s, fr);
    check(st.valid && st.blocksX == 8 && st.blocksY == 6, "grid", st.blocksX, 8);
    double sig = flux * 30e6 * 8.0;
    double wantVar = s.K(iso) * sig + s.read(iso) * s.read(iso);
    double gotVar = 0.5 * (st.siteTvar[1] + st.siteTvar[2]);
    check(std::fabs(gotVar / wantVar - 1) < 0.05, "temporal variance despite 2% flicker", gotVar, wantVar);
    check(std::fabs(st.siteMean[1] - (s.blackTrue[1] + sig)) < 0.03 * sig, "mean level", st.siteMean[1], s.blackTrue[1] + sig);
    double wantRow = std::pow(s.rowSigma50 * 8.0, 2) + wantVar / st.rowSamples[1];
    check(std::fabs(st.rowVar[1] / wantRow - 1) < 0.3, "row noise variance", st.rowVar[1], wantRow);
    check(st.defectCount == 0, "no false defects", st.defectCount, 0);
    check(st.svar[1][10] > wantVar / 4 * 0.8 && st.svar[1][10] < wantVar / 4 * 1.25, "spatial variance of burst mean",
          st.svar[1][10], wantVar / 4);

    // A blinking (RTS) pixel must not inflate its block's noise.
    for (size_t f = 0; f < fr.size(); ++f) {
        uint8_t* p = &fr[f][static_cast<size_t>(50) * s.stride() + (60 / 4) * 5];
        p[0] = static_cast<uint8_t>(f & 1 ? 40 : 200); // pixel (60, 50): 160 vs 800 DN
    }
    SensorStepStats rts = analyze(s, fr);
    int b = (50 - rts.y0) / 32 * rts.blocksX + (60 - rts.x0) / 32;
    int site = siteOf(s.cfa, 60, 50);
    check(rts.tvar[site][b] < wantVar * 1.15, "RTS pixel rejected from block noise", rts.tvar[site][b], wantVar);

    // Dark frames: black level, read noise, defects with gain-scaled excess.
    // (At ISO 400 the dark noise stays well above 0 DN; at higher ISOs the
    // sensor's clamp at 0 biases the mean up, which sensor.py corrects.)
    Sensor d;
    auto dark = d.burst(400, 30e6, 0.0, 4, rng);
    SensorStepStats ds = analyze(d, dark);
    for (int k = 0; k < 4; ++k)
        check(std::fabs(ds.siteMean[k] - d.blackTrue[k]) < 0.25, "dark mean = true black per site", ds.siteMean[k], d.blackTrue[k]);
    double rd2 = d.read(400) * d.read(400);
    check(std::fabs(ds.siteTvar[0] / rd2 - 1) < 0.06, "dark (read) noise", ds.siteTvar[0], rd2);
    int found = 0;
    for (const auto& px : d.defects) {
        if (px.dead) continue;
        for (const auto& e : ds.defects)
            if (e.x == px.x && e.y == px.y) {
                ++found;
                double want = px.hot50 * 8.0;
                check(std::fabs(e.excess - want) < 0.05 * want + 3, "hot pixel excess", e.excess, want);
            }
    }
    check(found == 3, "hot pixels found", found, 3);
    check(ds.defectCount == 3, "no other defects in the dark", ds.defectCount, 3);

    // Overexposed: histogram piles up at the true clip, not at 1023.
    auto over = d.burst(50, 30e6, 4000.0 / 30e6, 2, rng);
    SensorStepStats os = analyze(d, over);
    check(os.maxDn[1] == 1008, "max DN = sensor clip", os.maxDn[1], 1008);
    check(os.topHist[1][1008 - 896] > 1000, "clip pile-up bin", os.topHist[1][1008 - 896], 1000);
    check(os.clipped[1][0] == 0, "1008 is below reported white - 1 (no clip flags)", os.clipped[1][0], 0);

    // Metering helper.
    float bl[4] = {64, 64, 64, 64};
    double m = rawPatchMean(fr[0].data(), s.w, s.h, s.stride(), s.cfa, bl, 1023, 0.25, 0b0110);
    double want = (s.blackTrue[1] - 64 + sig) / (1023 - 64);
    check(std::fabs(m - want) < 0.01, "rawPatchMean", m, want);

    std::string j = sensorStepJson(st);
    check(j.front() == '{' && j.back() == '}' && j.find("\"tvar\":[[") != std::string::npos, "step JSON", 0, 0);
}

// --- DNG round trip: parse the IFD back --------------------------------------
static uint32_t rd(const std::vector<uint8_t>& b, size_t o, int n) {
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | b[o + i];
    return v;
}

static void testDng(const std::string& dir) {
    std::mt19937 rng(3);
    Sensor s;
    auto fr = s.burst(100, 20e6, 300.0 / 20e6, 1, rng);
    std::vector<float> map(17 * 13 * 4);
    for (int r = 0; r < 13; ++r)
        for (int c = 0; c < 17; ++c)
            for (int k = 0; k < 4; ++k) map[(r * 17 + c) * 4 + k] = static_cast<float>(1.0 + 0.1 * k + 0.01 * c);
    DngImageInfo d;
    d.width = s.w;
    d.height = s.h;
    d.rowStride = s.stride();
    d.cfa = s.cfa;
    for (int k = 0; k < 4; ++k) d.black[k] = static_cast<float>(64 + 0.5 * k);
    d.noiseProfile[0] = 1e-4;
    d.noiseProfile[1] = 2e-6;
    d.noiseChannels = 1;
    d.exposureNs = 20000000;
    d.iso = 100;
    d.model = "Test";
    d.shading = map.data();
    d.shadingCols = 17;
    d.shadingRows = 13;
    d.arrayWidth = static_cast<float>(s.w);
    d.arrayHeight = static_cast<float>(s.h);
    const std::string path = dir + "/test.dng";
    std::string err;
    check(writeDng(path, fr[0].data(), d, &err), "writeDng", 0, 1);
    FILE* f = std::fopen(path.c_str(), "rb");
    std::vector<uint8_t> b;
    if (f) {
        int c;
        while ((c = std::fgetc(f)) != EOF) b.push_back(static_cast<uint8_t>(c));
        std::fclose(f);
    }
    check(b.size() > static_cast<size_t>(s.w * s.h * 2) && b[0] == 'I' && rd(b, 2, 2) == 42, "TIFF header", b.size(), 0);
    if (b.size() < 16) return;
    uint32_t ifd = rd(b, 4, 4);
    int n = static_cast<int>(rd(b, ifd, 2));
    uint32_t width = 0, height = 0, strip = 0, prev = 0, opBytes = 0, opOff = 0;
    double blackPos1 = -1;
    bool sorted = true, haveNoise = false;
    for (int i = 0; i < n; ++i) {
        size_t e = ifd + 2 + 12 * i;
        uint32_t tag = rd(b, e, 2), count = rd(b, e + 4, 4), val = rd(b, e + 8, 4);
        sorted = sorted && tag > prev;
        prev = tag;
        if (tag == 256) width = val;
        if (tag == 257) height = val;
        if (tag == 273) strip = val;
        if (tag == 50714) blackPos1 = static_cast<double>(rd(b, val + 8, 4)) / rd(b, val + 12, 4); // 2nd entry: (x1, y0)
        if (tag == 51009) { opBytes = count; opOff = val; }
        if (tag == 51041) haveNoise = count == 6;
    }
    check(sorted, "IFD tags sorted", 0, 1);
    check(width == static_cast<uint32_t>(s.w) && height == static_cast<uint32_t>(s.h), "DNG size", width, s.w);
    check(haveNoise, "NoiseProfile per plane", haveNoise, 1);
    // GRBG: position (1, 0) is R -> logical site 0 -> 64.0.
    check(std::fabs(blackPos1 - 64.0) < 1e-6, "BlackLevel in CFA order", blackPos1, 64.0);
    check(opBytes == 4 + 4 * (16 + 76 + 4 * 17 * 13), "GainMap opcode list size", opBytes, 4 + 4 * (16 + 76 + 4 * 17 * 13));
    if (opOff && opOff + 40 < b.size()) {
        auto be = [&](size_t o) { return (uint32_t(b[o]) << 24) | (uint32_t(b[o + 1]) << 16) | (uint32_t(b[o + 2]) << 8) | b[o + 3]; };
        check(be(opOff) == 4 && be(opOff + 4) == 9, "4 GainMap opcodes", be(opOff), 4);
    }
    // Pixel (x=5, y=3) round trip.
    if (strip && strip + static_cast<size_t>(s.w) * s.h * 2 <= b.size()) {
        const uint8_t* p = &fr[0][static_cast<size_t>(3) * s.stride() + (5 / 4) * 5];
        int want = (p[1] << 2) | ((p[4] >> 2) & 3);
        int got = static_cast<int>(rd(b, strip + (static_cast<size_t>(3) * s.w + 5) * 2, 2));
        check(got == want, "DNG pixel data", got, want);
    }
}

// --- Synthetic sweeps for the Python analysis --------------------------------
static std::string arr(const double* v, int n) {
    std::string s = "[";
    char b[40];
    for (int i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%s%.9g", i ? "," : "", v[i]);
        s += b;
    }
    return s + "]";
}

static void writeSweep(const std::string& dir, bool white) {
    std::mt19937 rng(white ? 11 : 13);
    Sensor s;
    const double range = 1023 - s.blackReported;
    std::string steps;
    auto addStep = [&](int iso, double expNs, int stop, bool ladder, double flux) {
        auto fr = s.burst(iso, expNs, flux, 4, rng);
        SensorStepStats st = analyze(s, fr);
        // The HAL's profile: S right, O slightly negative (as the Pixel reports).
        double S = s.K(iso) / range, O = -1e-7;
        double np[8] = {S, O, S, O, S, O, S, O};
        double bl[4] = {s.blackReported, s.blackReported, s.blackReported, s.blackReported};
        char b[512];
        std::snprintf(b, sizeof(b), "%s{\"requestedIso\":%d,\"requestedExposureNs\":%.0f,\"stop\":%d,\"ladder\":%s,\"shading\":0,\"ref\":false,"
                      "\"iso\":%d,\"exposureNs\":%.0f,\"frameDurationNs\":33333333,\"analogIso\":%d,\"digitalGain\":1,"
                      "\"whiteLevel\":1023,\"noiseS\":%.9g,\"noiseO\":%.9g,\"engineNoiseS\":%.9g,\"engineNoiseO\":2e-06,\"noiseChannels\":4,",
                      steps.empty() ? "" : ",", iso, expNs, stop, ladder ? "true" : "false", iso, expNs, iso, S, O, S);
        steps += b;
        steps += "\"blackLevel\":" + arr(bl, 4) + ",\"noiseProfile\":" + arr(np, 8) + ",\"noiseProfileSites\":" + arr(np, 8) +
                 ",\"halNeutral\":[0.55,1,0.7],\"stats\":" + sensorStepJson(st) + "}";
    };
    const int isos[] = {50, 100, 200, 400, 800, 1600};
    const double flux50 = 480.0 / 2e6; // green centre at half range (~480 DN) at ISO 50, 2 ms
    if (!white) {
        for (int iso : isos) {
            addStep(iso, 30e6, 0, false, 0.0);
            addStep(iso, 1e6, 0, false, 0.0);
        }
    } else {
        for (int k = 3; k >= -8; --k) addStep(50, std::ldexp(2e6, k), k, true, flux50);
        for (int iso : isos) {
            if (iso == 50) continue;
            for (int k : {2, 0, -2, -4, -6}) addStep(iso, std::ldexp(2e6 * 50 / iso, k), k, false, flux50);
        }
    }
    std::string map = "[";
    const int cols = 17, rows = 13;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            for (int k = 0; k < 4; ++k) {
                char b[32];
                std::snprintf(b, sizeof(b), "%s%.6g", (r || c || k) ? "," : "",
                              s.mapGain(k, c * s.w / (cols - 1.0), r * s.h / (rows - 1.0)));
                map += b;
            }
    map += "]";
    double readTable[6], kTable[6];
    for (int i = 0; i < 6; ++i) {
        kTable[i] = s.K(isos[i]) / range;
        readTable[i] = std::pow(s.read(isos[i]) / range, 2);
    }
    char b[1024];
    std::snprintf(b, sizeof(b), "{\"format\":\"vesper-sensor-calibration/1\",\"kind\":\"%s\",\"device\":\"Synthetic\",\"cameraId\":\"0\","
                  "\"width\":%d,\"height\":%d,\"fps\":30,\"burst\":4,\"rotation\":90,\"ladderIso\":50,\"meteredHalfNs\":2000000,"
                  "\"cfa\":%d,\"arrayWidth\":%d,\"arrayHeight\":%d,\"minIso\":50,\"maxIso\":1600,\"maxAnalogIso\":1600,"
                  "\"minExposureNs\":10000,\"maxExposureNs\":1000000000,\"staticWhiteLevel\":1023,\"staticBlackLevel\":[64,64,64,64],"
                  "\"sensorMap\":[1,1,0,0],\"warnings\":[],",
                  white ? "white" : "dark", s.w, s.h, s.cfa, s.w, s.h);
    std::string j = b;
    j += "\"shadingMaps\":[{\"cols\":17,\"rows\":13,\"map\":" + map + "}],\"steps\":[" + steps + "],";
    double vig[4] = {s.vig[0], s.vig[1], s.vig[2], s.vig[3]};
    std::snprintf(b, sizeof(b), "\"synthTruth\":{\"black\":%s,\"clip\":%.0f,\"mapErrorR\":%.4f,\"hot\":%zu,\"isos\":[50,100,200,400,800,1600],"
                  "\"S\":%s,\"O\":%s,\"vignetting\":%s}}",
                  arr(s.blackTrue, 4).c_str(), s.satTrue, s.mapError, s.defects.size() - 1, arr(kTable, 6).c_str(),
                  arr(readTable, 6).c_str(), arr(vig, 4).c_str());
    j += b;
    std::string path = dir + (white ? "/VSENSOR_white_synthetic.json" : "/VSENSOR_dark_synthetic.json");
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { check(false, "write synthetic sweep", 0, 1); return; }
    std::fwrite(j.data(), 1, j.size(), f);
    std::fclose(f);
}

int main(int argc, char** argv) {
    testStatistics();
    std::string dir = argc > 1 ? argv[1] : "/tmp";
    testDng(dir);
    if (argc > 1) {
        writeSweep(dir, false);
        writeSweep(dir, true);
    }
    if (failures) {
        std::printf("%d sensor calibration test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all sensor calibration statistics / DNG tests passed\n");
    return 0;
}
