#include "gyroflow.h"

#include <cmath>
#include <cstdio>

namespace vesper {

namespace {

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

// A direction in Android device axes (x right, y up) as one of the four
// unit vectors; the app's rotation is clockwise.
struct Dir { int x, y; };
Dir rotCw(Dir d, int degrees) {
    for (int i = 0; i < ((degrees % 360) + 360) % 360 / 90; ++i) d = {d.y, -d.x};
    return d;
}
// Dir (x,y) -> letter for the IMU axis it lies along; lower case = negative.
char letter(Dir d) {
    if (d.x == 1) return 'X';
    if (d.x == -1) return 'x';
    if (d.y == 1) return 'Y';
    return 'y';
}

} // namespace

std::string sanitizeHeaderValue(const std::string& v) {
    std::string o = v;
    for (char& c : o)
        if (c == ',' || c == '\n' || c == '\r') c = ' ';
    return o;
}

std::string gcsvHeader(const GcsvHeader& h) {
    // No `lensprofile` line: Gyroflow looks that name up in its own lens
    // database, never as a file next to the clip.
    std::string s = "GYROFLOW IMU LOG\nversion,1.3\n";
    s += "id," + sanitizeHeaderValue(h.id) + "\n";
    s += "orientation," + sanitizeHeaderValue(h.orientation) + "\n";
    if (!h.note.empty()) s += "note," + sanitizeHeaderValue(h.note) + "\n";
    if (!h.vendor.empty()) s += "vendor," + sanitizeHeaderValue(h.vendor) + "\n";
    if (!h.videoFilename.empty()) s += "videofilename," + sanitizeHeaderValue(h.videoFilename) + "\n";
    // Android accelerometer values are m/s^2; Gyroflow multiplies by ascale to get g.
    s += "tscale,0.000001\ngscale,1\nascale,0.101971621\n";
    if (h.readoutMs > 0) {
        char b[96];
        std::snprintf(b, sizeof(b), "frame_readout_time,%.4f\nframe_readout_direction,%s\n", h.readoutMs,
                      h.readoutDirection.c_str());
        s += b;
    }
    s += "t,gx,gy,gz,ax,ay,az\n";
    return s;
}

std::string gcsvRow(const GyroSample& g, int64_t t0BoottimeNs) {
    const int64_t dt = g.tsNs - t0BoottimeNs;
    if (dt < -kPreRollNs) return "";
    char b[160];
    std::snprintf(b, sizeof(b), "%lld,%.6f,%.6f,%.6f,%.4f,%.4f,%.4f\n", static_cast<long long>(std::llround(dt / 1000.0)),
                  g.gyro[0], g.gyro[1], g.gyro[2], g.accel[0], g.accel[1], g.accel[2]);
    return b;
}

std::string gcsvOrientation(int sensorOrientation, int outputRotation) {
    // Rotating the raw image clockwise by the sensor orientation makes it
    // upright on the natural-portrait screen, where right = +x and down = -y
    // (device axes). Rotating the raw image clockwise by the app's rotation
    // as well, the video's right/down axes lie along the portrait right/down
    // vectors turned clockwise by (sensorOrientation - outputRotation).
    // Pixel back camera (sensor 90), landscape with the display rotated 90:
    // output rotation 0 -> "yxz"; display 270: rotation 180 -> "YXz";
    // natural portrait (rotation 90) -> "Xyz". Forward (the way the lens
    // looks) is -z on the back camera, hence the lower case z.
    const int turn = sensorOrientation - outputRotation;
    std::string o;
    o += letter(rotCw({1, 0}, turn));
    o += letter(rotCw({0, -1}, turn));
    o += 'z';
    return o;
}

bool validOrientation(const std::string& o) {
    if (o.size() != 3) return false;
    int seen = 0;
    for (char ch : o) {
        switch (ch) {
            case 'x': case 'X': seen |= 1; break;
            case 'y': case 'Y': seen |= 2; break;
            case 'z': case 'Z': seen |= 4; break;
            default: return false;
        }
    }
    return seen == 7;
}

const char* readoutDirectionFor(int outputRotation) {
    switch (((outputRotation % 360) + 360) % 360) {
        case 90: return "RightToLeft";   // raw rows run left-to-right, rotated clockwise: top rows end up right
        case 180: return "BottomToTop";
        case 270: return "LeftToRight";
        default: return "TopToBottom";
    }
}

double readoutFraction(double cropH, double rawH) {
    // The sensor reads raw rows top to bottom whatever the output rotation, so
    // the fraction is always along raw y.
    if (rawH <= 0) return 1.0;
    return std::fmin(1.0, std::fmax(0.0, cropH / rawH));
}

OutputIntrinsics outputIntrinsics(const float f[4], const float map[4], const float crop[4], int outW, int outH, int rot) {
    // rawPosFor() of render.comp with c = continuous output coordinate.
    auto rawFor = [&](double cx, double cy, double& rx, double& ry) {
        double ux, uy, udx, udy;
        if (rot == 90)       { udx = outH; udy = outW; ux = cy; uy = outW - cx; }
        else if (rot == 180) { udx = outW; udy = outH; ux = outW - cx; uy = outH - cy; }
        else if (rot == 270) { udx = outH; udy = outW; ux = outH - cy; uy = cx; }
        else                 { udx = outW; udy = outH; ux = cx; uy = cy; }
        rx = crop[0] + ux * crop[2] / udx;
        ry = crop[1] + uy * crop[3] / udy;
    };
    // array = raw * s + off: an affine map output -> array, A * o + t.
    double p0x, p0y, pxx, pxy, pyx, pyy;
    rawFor(0, 0, p0x, p0y);
    rawFor(1, 0, pxx, pxy);
    rawFor(0, 1, pyx, pyy);
    const double a00 = (pxx - p0x) * map[0], a10 = (pxy - p0y) * map[1];
    const double a01 = (pyx - p0x) * map[0], a11 = (pyy - p0y) * map[1];
    const double tx = p0x * map[0] + map[2], ty = p0y * map[1] + map[3];
    // Inverse: array -> output.
    const double det = a00 * a11 - a01 * a10;
    OutputIntrinsics k;
    if (std::fabs(det) < 1e-12) return k;
    const double i00 = a11 / det, i01 = -a01 / det, i10 = -a10 / det, i11 = a00 / det;
    // Principal point: where the array point (cx, cy) lands. Android's pixel
    // coordinates have the pixel's corner at integers (the shader samples at
    // +0.5), OpenCV's have its centre: subtract 0.5.
    const double dx = f[2] - tx, dy = f[3] - ty;
    k.cx = i00 * dx + i01 * dy - 0.5;
    k.cy = i10 * dx + i11 * dy - 0.5;
    // Focal lengths: output pixels per unit of normalised ray offset. The
    // map is a rotation by a multiple of 90 degrees plus a scale, so one
    // entry of each row is zero.
    k.fx = std::fabs(i00 * f[0]) + std::fabs(i01 * f[1]);
    k.fy = std::fabs(i10 * f[0]) + std::fabs(i11 * f[1]);
    return k;
}

bool outputDistortion(const float d[5], int rot, double out[5]) {
    const int r = ((rot % 360) + 360) % 360;
    const double k1 = d[0], k2 = d[1], k3 = d[2], p1 = d[3], p2 = d[4];
    if (r == 0)        { out[0] = k1; out[1] = k2; out[2] = p1;  out[3] = p2;  out[4] = k3; return true; }
    if (r == 180)      { out[0] = k1; out[1] = k2; out[2] = -p1; out[3] = -p2; out[4] = k3; return true; }
    for (int i = 0; i < 5; ++i) out[i] = 0;
    return false;
}

std::string lensProfileJson(const LensProfileInput& in) {
    char b[1024];
    std::string j = "{\n";
    j += "  \"name\": \"" + jsonEscape(in.brand + " " + in.model + " Vesper Cine " + in.cropMode + " " +
                                      std::to_string(in.width) + "x" + std::to_string(in.height)) + "\",\n";
    j += "  \"note\": \"" + jsonEscape(in.note) + "\",\n";
    j += "  \"calibrated_by\": \"Vesper Cine " + jsonEscape(in.appVersion) + " (from the camera's own calibration, no checkerboard)\",\n";
    j += "  \"camera_brand\": \"" + jsonEscape(in.brand) + "\",\n";
    j += "  \"camera_model\": \"" + jsonEscape(in.model) + "\",\n";
    j += "  \"lens_model\": \"main " + jsonEscape(in.cropMode) + (in.lensCorrected ? ", lens corrected" : ", uncorrected") + "\",\n";
    j += "  \"camera_setting\": \"\",\n";
    std::snprintf(b, sizeof(b),
                  "  \"calib_dimension\": {\"w\": %d, \"h\": %d},\n  \"orig_dimension\": {\"w\": %d, \"h\": %d},\n"
                  "  \"output_dimension\": {\"w\": %d, \"h\": %d},\n  \"frame_readout_time\": %.4f,\n"
                  "  \"fps\": %.3f,\n  \"official\": false,\n  \"asymmetrical\": false,\n  \"num_images\": 0,\n"
                  "  \"calibrator_version\": \"vesper-cine\",\n  \"global_shutter\": false,\n  \"distortion_model\": \"opencv_standard\",\n",
                  in.width, in.height, in.width, in.height, in.width, in.height, in.readoutMs, in.fps);
    j += b;
    std::snprintf(b, sizeof(b),
                  "  \"fisheye_params\": {\n    \"RMS_error\": 0,\n"
                  "    \"camera_matrix\": [[%.4f, 0.0, %.4f], [0.0, %.4f, %.4f], [0.0, 0.0, 1.0]],\n"
                  "    \"distortion_coeffs\": [%.9g, %.9g, %.9g, %.9g, %.9g, 0.0, 0.0, 0.0]\n  }\n}\n",
                  in.k.fx, in.k.cx, in.k.fy, in.k.cy, in.dist[0], in.dist[1], in.dist[2], in.dist[3], in.dist[4]);
    j += b;
    return j;
}

} // namespace vesper
