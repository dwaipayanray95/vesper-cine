#pragma once
// Gyroflow motion data (0.28.0): pure logic, no Android headers, so the host
// tests can run it. The IMU thread is in imu_logger.{h,cpp}.
//
// Formats, as written from the Gyroflow documentation (docs.gyroflow.xyz
// "GCSV format" and the lens_profiles repository; the sandbox that wrote this
// code had no network, so the field list is from memory and must be checked
// against a real Gyroflow install, see CLAUDE.md "Gyroflow"):
//  * GCSV: first line "GYROFLOW IMU LOG", "key,value" header lines, then the
//    column line "t,gx,gy,gz,ax,ay,az" and one row per sample. `tscale`
//    multiplies the time column to seconds, `gscale` the gyro columns to rad/s,
//    `ascale` the accelerometer columns to g (we write m/s^2, so 1/9.80665). `orientation` is three
//    letters, one per VIDEO axis (X right, Y down, Z forward = OpenCV camera
//    frame), naming the IMU axis that lies along it; lower case = negated.
//    `frame_readout_time` is in milliseconds, `frame_readout_direction` one of
//    TopToBottom, BottomToTop, LeftToRight, RightToLeft.
//  * Lens profile: JSON with camera_brand, camera_model, calib_dimension,
//    orig_dimension, output_dimension, frame_readout_time, distortion_model
//    "opencv_standard" (coefficients k1, k2, p1, p2, k3) and
//    fisheye_params.camera_matrix (3x3) + distortion_coeffs.

#include <cstdint>
#include <string>

namespace vesper {

// Camera frame timestamps are CLOCK_BOOTTIME when the sensor's timestamp
// source is REALTIME, else CLOCK_MONOTONIC; Android sensor events are always
// CLOCK_BOOTTIME. `boottimeMinusMonotonicNs` = clock_gettime(BOOTTIME) -
// clock_gettime(MONOTONIC), measured once at the start of a take.
inline int64_t toBoottimeNs(int64_t cameraNs, bool cameraIsRealtime, int64_t boottimeMinusMonotonicNs) {
    return cameraIsRealtime ? cameraNs : cameraNs + boottimeMinusMonotonicNs;
}

struct GyroSample {
    int64_t tsNs = 0;     // CLOCK_BOOTTIME
    float gyro[3] = {0, 0, 0};  // rad/s, Android device axes
    float accel[3] = {0, 0, 0}; // m/s^2, Android device axes
};

struct GcsvHeader {
    std::string id = "vesper_cine";
    std::string vendor;          // "Vesper Cine 0.28.0 / Google Pixel 10"
    std::string videoFilename;
    std::string orientation;     // see gcsvOrientation()
    std::string note;
    double readoutMs = 0;        // 0 = unknown, then the line is omitted
    std::string readoutDirection = "TopToBottom";
};

// Commas and line breaks would corrupt a "key,value" header line: replaced by spaces.
std::string sanitizeHeaderValue(const std::string& v);
std::string gcsvHeader(const GcsvHeader& h); // includes the column line
// One row, time in microseconds since `t0BoottimeNs` (frame 0's start of
// exposure). Samples up to kPreRollNs before t0 get a negative time (Gyroflow
// parses t as a float); earlier ones return "".
constexpr int64_t kPreRollNs = 500'000'000;
std::string gcsvRow(const GyroSample& s, int64_t t0BoottimeNs);

// Image-axis mapping. `sensorOrientation`: ACAMERA_SENSOR_ORIENTATION;
// `outputRotation`: the clockwise degrees the app rotates the raw frame by
// (rotationDegrees() in native_bridge: sensorOrientation - display rotation).
// Android device axes: x right, y up the screen, z out of the screen (natural
// portrait); the back camera looks along -z. Result e.g. "yxz".
std::string gcsvOrientation(int sensorOrientation, int outputRotation);
// Which way the sensor's row-by-row readout runs in the output image.
const char* readoutDirectionFor(int outputRotation);
// Fraction of the sensor readout (rolling-shutter skew covers the whole raw
// height) that the output's crop spans: cropH / rawH in raw rows.
double readoutFraction(double cropH, double rawH);

// The logger writes `<name>.gcsv.part` and renames it to `<name>.gcsv` when complete.
inline std::string partPathFor(const std::string& finalPath) { return finalPath + ".part"; }
// A valid IMU orientation string: the letters X, Y, Z once each, any case.
bool validOrientation(const std::string& o);

struct OutputIntrinsics { double fx = 0, fy = 0, cx = 0, cy = 0; }; // output px, OpenCV convention (centre of the first pixel = 0,0)
// arrayIntrinsics: ACAMERA_LENS_INTRINSIC_CALIBRATION fx, fy, cx, cy in
// pre-correction array pixels. sensorMap = [sx, sy, offX, offY] (array =
// raw * s + off, as FrameParams::sensorMap), cropRect = [x, y, w, h] in raw
// px (after the lens-correction shrink), rot as rawPosFor() in render.comp.
OutputIntrinsics outputIntrinsics(const float arrayIntrinsics[4], const float sensorMap[4], const float cropRect[4],
                                  int outW, int outH, int rot);
// Brown-Conrady (k1, k2, k3, p1, p2 from ACAMERA_LENS_DISTORTION) re-expressed
// for the output orientation, in OpenCV order k1, k2, p1, p2, k3. Rotations of
// 90/270 are not handled (landscape only): zeros and false.
bool outputDistortion(const float lensDistortion[5], int rot, double outCoeffs[5]);

struct LensProfileInput {
    std::string brand, model, note, appVersion;
    int width = 0, height = 0;
    double fps = 0, readoutMs = 0;
    OutputIntrinsics k;
    double dist[5] = {0, 0, 0, 0, 0}; // k1, k2, p1, p2, k3
    bool lensCorrected = true;
    std::string cropMode;             // "16:9" / "4:3 open gate"
};
std::string lensProfileJson(const LensProfileInput& in);

} // namespace vesper
