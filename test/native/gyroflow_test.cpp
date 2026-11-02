// Gyroflow: GCSV header/rows, time alignment, orientation, lens intrinsics.
#include "gyroflow.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace vesper;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, tol) do { double _a = (a), _b = (b); if (std::fabs(_a - _b) > (tol)) { std::printf("FAIL line %d: %s = %g, expected %g\n", __LINE__, #a, _a, _b); ++failures; } } while (0)

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

int main() {
    // Time base: REALTIME cameras are BOOTTIME already; MONOTONIC ones get the measured offset.
    CHECK(toBoottimeNs(1000, true, 555) == 1000);
    CHECK(toBoottimeNs(1000, false, 555) == 1555);

    // Header.
    GcsvHeader h;
    h.vendor = "Vesper Cine 0.28.0 / Google Pixel 10";
    h.videoFilename = "VESPER_20261102_101500.mp4";
    h.orientation = "yxz";
    h.readoutMs = 13.25;
    h.readoutDirection = "TopToBottom";
    std::string hd = gcsvHeader(h);
    CHECK(hd.rfind("GYROFLOW IMU LOG\n", 0) == 0);
    CHECK(has(hd, "\nversion,1.3\n"));
    CHECK(has(hd, "\norientation,yxz\n"));
    CHECK(has(hd, "\nvideofilename,VESPER_20261102_101500.mp4\n"));
    CHECK(!has(hd, "lensprofile")); // Gyroflow only looks that up in its own database
    CHECK(has(hd, "\nascale,0.101971621\n")); // m/s^2 -> g
    CHECK(has(hd, "\ngscale,1\n"));
    CHECK(has(hd, "\ntscale,0.000001\n"));
    CHECK(has(hd, "\nframe_readout_time,13.2500\n"));
    CHECK(hd.size() > 20 && hd.compare(hd.size() - 20, 20, "t,gx,gy,gz,ax,ay,az\n") == 0);
    GcsvHeader h0;
    CHECK(!has(gcsvHeader(h0), "frame_readout_time")); // unknown readout: line omitted

    // Rows: t = 0 at frame 0's start of exposure. Camera clock MONOTONIC, sensor events BOOTTIME.
    const int64_t offset = 7'000'000'000LL;           // BOOTTIME - MONOTONIC
    const int64_t frame0Camera = 100'000'000'000LL;   // MONOTONIC
    const int64_t t0 = toBoottimeNs(frame0Camera, false, offset);
    GyroSample s;
    s.tsNs = t0 + 2'500'000;                          // 2.5 ms after frame 0
    s.gyro[0] = 0.5f; s.gyro[1] = -0.25f; s.gyro[2] = 0.125f;
    s.accel[0] = 0.1f; s.accel[1] = 9.81f; s.accel[2] = -0.2f;
    CHECK(gcsvRow(s, t0) == "2500,0.500000,-0.250000,0.125000,0.1000,9.8100,-0.2000\n");
    s.tsNs = t0;
    CHECK(gcsvRow(s, t0).rfind("0,", 0) == 0);
    // Pre-roll: up to 0.5 s before frame 0 is kept with a negative time.
    s.tsNs = t0 - 2'000'000;
    CHECK(gcsvRow(s, t0).rfind("-2000,", 0) == 0);
    s.tsNs = t0 - 500'000'000;
    CHECK(gcsvRow(s, t0).rfind("-500000,", 0) == 0);
    s.tsNs = t0 - 500'000'001;
    CHECK(gcsvRow(s, t0).empty());                    // older than the pre-roll: not written
    // Wrong clock would be off by the offset (7 s): make sure the conversion matters.
    s.tsNs = frame0Camera + 2'500'000;
    CHECK(gcsvRow(s, t0).empty());

    // Header values can't break the key,value lines.
    {
        GcsvHeader bad;
        bad.id = "a,b\nc";
        bad.vendor = "V,\r\nX";
        bad.videoFilename = "clip,1.mp4";
        bad.note = "x\ny,z";
        bad.orientation = "yxz";
        std::string o = gcsvHeader(bad);
        CHECK(has(o, "\nid,a b c\n"));
        CHECK(has(o, "\nvendor,V   X\n"));
        CHECK(has(o, "\nvideofilename,clip 1.mp4\n"));
        CHECK(has(o, "\nnote,x y z\n"));
        CHECK(sanitizeHeaderValue("a,b") == "a b");
    }
    // .part naming and orientation validation (developer override).
    CHECK(partPathFor("/x/VESPER_1.gcsv") == "/x/VESPER_1.gcsv.part");
    CHECK(validOrientation("yxz") && validOrientation("YXZ") && validOrientation("zXy"));
    CHECK(!validOrientation("xxz") && !validOrientation("xy") && !validOrientation("xyzz") && !validOrientation("abc") && !validOrientation(""));

    // Orientation (Pixel back camera, sensor orientation 90).
    CHECK(gcsvOrientation(90, 0) == "yxz");   // display ROTATION_90 (phone top to the left)
    CHECK(gcsvOrientation(90, 180) == "YXz"); // display ROTATION_270
    CHECK(gcsvOrientation(90, 90) == "Xyz");  // natural portrait
    CHECK(std::string(readoutDirectionFor(0)) == "TopToBottom");
    CHECK(std::string(readoutDirectionFor(180)) == "BottomToTop");
    NEAR(readoutFraction(2256, 2256), 1.0, 1e-12);
    NEAR(readoutFraction(1500, 3000), 0.5, 1e-12);

    // Lens intrinsics, synthetic camera: 4000x3000 array, f = 3000 px, centre (2000,1500);
    // readout 4000x2256 (16:9 centred vertical crop, offset (3000-2256)/2 = 372).
    {
        const float f[4] = {3000, 3000, 2000, 1500};
        const float map[4] = {1, 1, 0, 372};
        // Crop = whole raw width, 2250 rows (16:9 of 4000 = 2250), centred: rows 3..2253.
        const float crop[4] = {0, 3, 4000, 2250};
        OutputIntrinsics k = outputIntrinsics(f, map, crop, 1920, 1080, 0);
        NEAR(k.fx, 3000.0 * 1920 / 4000, 1e-6); // 1440
        NEAR(k.fy, 3000.0 * 1080 / 2250, 1e-6); // 1440
        // cx: array 2000 -> raw 2000 -> output 960; OpenCV centre convention: 959.5.
        NEAR(k.cx, 959.5, 1e-6);
        // cy: array 1500 -> raw 1128 -> (1128-3)/2250*1080 = 540 -> 539.5.
        NEAR(k.cy, 539.5, 1e-6);
        // Rotated 180 (display 270): the principal point mirrors, focal lengths stay.
        OutputIntrinsics k2 = outputIntrinsics(f, map, crop, 1920, 1080, 180);
        NEAR(k2.fx, 1440, 1e-6);
        NEAR(k2.cx, 1920 - 959.5 - 1, 1e-6); // 959.5 again: centred map
        // Off-centre optical centre shifts and mirrors.
        const float f3[4] = {3000, 3000, 2100, 1500};
        OutputIntrinsics k3 = outputIntrinsics(f3, map, crop, 1920, 1080, 0);
        OutputIntrinsics k4 = outputIntrinsics(f3, map, crop, 1920, 1080, 180);
        NEAR(k3.cx, 959.5 + 100.0 * 1920 / 4000, 1e-6);
        NEAR(k3.cx + k4.cx, 1919.0, 1e-6);
        // Binned readout (2000x1128 raw, array = 2 x raw): focal length in output px halves per 2x scale.
        const float mapB[4] = {2, 2, 0, 372};
        const float cropB[4] = {0, 1.5f, 2000, 1125};
        OutputIntrinsics kb = outputIntrinsics(f, mapB, cropB, 1920, 1080, 0);
        NEAR(kb.fx, 3000.0 * 1920 / 4000, 1e-6);
        // Lens correction shrink (crop 10 % smaller): zoomed in, focal length grows by 1.1.
        const float cropS[4] = {200, 3 + 112.5f, 3600, 2025};
        OutputIntrinsics ks = outputIntrinsics(f, map, cropS, 1920, 1080, 0);
        NEAR(ks.fx, 1440 * 1.1111111, 1e-3);
        NEAR(ks.cx, 959.5, 1e-6);
        // Portrait style rotation 90 into a 1080x1920 output swaps the roles.
        const float cropP[4] = {0, 3, 4000, 2250};
        OutputIntrinsics kp = outputIntrinsics(f, map, cropP, 1080, 1920, 90);
        NEAR(kp.fx, 3000.0 * 1080 / 2250, 1e-6);
        NEAR(kp.fy, 3000.0 * 1920 / 4000, 1e-6);
    }

    // Distortion terms by rotation.
    {
        const float d[5] = {0.1f, -0.02f, 0.003f, 0.001f, -0.002f}; // k1 k2 k3 p1 p2
        double o[5];
        CHECK(outputDistortion(d, 0, o));
        NEAR(o[0], 0.1, 1e-7); NEAR(o[2], 0.001, 1e-7); NEAR(o[3], -0.002, 1e-7); NEAR(o[4], 0.003, 1e-7);
        CHECK(outputDistortion(d, 180, o));
        NEAR(o[2], -0.001, 1e-7); NEAR(o[3], 0.002, 1e-7);
        CHECK(!outputDistortion(d, 90, o));
    }

    // Lens profile JSON carries the numbers.
    {
        LensProfileInput in;
        in.brand = "Google"; in.model = "Pixel 10"; in.appVersion = "0.28.0";
        in.width = 1920; in.height = 1080; in.fps = 24; in.readoutMs = 12.5;
        in.k = {1440, 1440, 959.5, 539.5};
        in.cropMode = "16:9";
        std::string j = lensProfileJson(in);
        CHECK(has(j, "\"camera_brand\": \"Google\""));
        CHECK(has(j, "\"distortion_model\": \"opencv_standard\""));
        CHECK(has(j, "[[1440.0000, 0.0, 959.5000], [0.0, 1440.0000, 539.5000], [0.0, 0.0, 1.0]]"));
        CHECK(has(j, "\"frame_readout_time\": 12.5000"));
        CHECK(has(j, "\"calib_dimension\": {\"w\": 1920, \"h\": 1080}"));
    CHECK(has(j, "\"global_shutter\": false"));
    CHECK(has(j, "\"distortion_coeffs\": [0, 0, 0, 0, 0, 0.0, 0.0, 0.0]")); // 8 coefficients
    }

    if (failures) { std::printf("%d gyroflow test(s) FAILED\n", failures); return 1; }
    std::printf("all gyroflow tests passed\n");
    return 0;
}
