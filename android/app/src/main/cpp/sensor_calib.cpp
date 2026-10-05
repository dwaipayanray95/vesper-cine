#include "sensor_calib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vesper {
namespace {

// Logical site 0=R 1=Gr 2=Gb 3=B at raw parity (px, py), as camera_engine.h cfaSite().
int siteOf(int cfa, int px, int py) {
    int bit = ((py & 1) << 1) | (px & 1);
    switch (cfa) {
        case 0: return bit;
        case 1: return bit ^ 1;
        case 2: return bit ^ 2;
        default: return 3 - bit;
    }
}

void unpackRow(const uint8_t* row, int width, uint16_t* out) {
    for (int g = 0; g < width / 4; ++g) {
        const uint8_t* p = row + g * 5;
        for (int i = 0; i < 4; ++i) out[g * 4 + i] = static_cast<uint16_t>((p[i] << 2) | ((p[4] >> (2 * i)) & 3));
    }
}

void appendNum(std::string& s, double v, const char* fmt = "%.6g") {
    char b[32];
    if (!std::isfinite(v)) v = 0;
    std::snprintf(b, sizeof(b), fmt, v);
    s += b;
}

template <typename T>
void appendArray(std::string& s, const T* v, size_t n, const char* fmt = "%.6g") {
    s += '[';
    for (size_t i = 0; i < n; ++i) {
        if (i) s += ',';
        appendNum(s, static_cast<double>(v[i]), fmt);
    }
    s += ']';
}

template <typename T>
void appendSites(std::string& s, const char* key, const std::vector<T> (&v)[4], const char* fmt = "%.6g") {
    s += '"';
    s += key;
    s += "\":[";
    for (int k = 0; k < 4; ++k) {
        if (k) s += ',';
        appendArray(s, v[k].data(), v[k].size(), fmt);
    }
    s += ']';
}

} // namespace

SensorStepStats analyzeSensorStep(const SensorStepInput& in) {
    SensorStepStats s;
    const int W = in.width & ~3, H = in.height & ~1;
    const int N = static_cast<int>(in.frames.size());
    const int B = in.block & ~3;
    if (W < 16 || H < 16 || N < 2 || N > 64 || B < 4 || in.rowStride < W * 5 / 4) return s;
    s.frames = N;
    s.pairs = N / 2;
    s.block = B;
    s.blocksX = W / B;
    s.blocksY = H / B;
    if (s.blocksX == 0 || s.blocksY == 0) return s;
    s.x0 = ((W - s.blocksX * B) / 2) & ~1;
    s.y0 = ((H - s.blocksY * B) / 2) & ~1;
    const int P = s.pairs, nb = s.blocksX * s.blocksY;
    const int gx1 = s.x0 + s.blocksX * B, gy1 = s.y0 + s.blocksY * B;
    const double perSite = (B / 2.0) * (B / 2.0);
    const float sat = in.white - 1.0f;

    std::vector<double> sum[4], dsum[4], d2sum[4];
    for (int k = 0; k < 4; ++k) {
        sum[k].assign(nb, 0.0);
        dsum[k].assign(static_cast<size_t>(nb) * P, 0.0);
        d2sum[k].assign(static_cast<size_t>(nb) * P, 0.0);
        s.clipped[k].assign(nb, 0);
        s.topHist[k].assign(128, 0);
    }
    std::vector<uint16_t> burst(static_cast<size_t>(W) * H); // sum of the N frames per pixel
    std::vector<double> rowD(static_cast<size_t>(P) * H * 2, 0.0);   // [p][y][x parity]: sum of pair differences
    std::vector<double> colD(static_cast<size_t>(P) * 2 * W, 0.0);   // [p][y parity][x]
    std::vector<std::vector<uint16_t>> rows(N, std::vector<uint16_t>(W));

    // Pass 1: block sums, pair differences, clip histogram, burst sum.
    for (int y = 0; y < H; ++y) {
        for (int f = 0; f < N; ++f) unpackRow(in.frames[f] + static_cast<size_t>(y) * in.rowStride, W, rows[f].data());
        uint16_t* bRow = burst.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            int t = 0;
            for (int f = 0; f < N; ++f) t += rows[f][x];
            bRow[x] = static_cast<uint16_t>(t);
        }
        if (y < s.y0 || y >= gy1) continue;
        const int by = (y - s.y0) / B, py = y & 1;
        for (int x = s.x0; x < gx1; ++x) {
            const int site = siteOf(in.cfa, x, py);
            const int b = by * s.blocksX + (x - s.x0) / B;
            for (int f = 0; f < N; ++f) {
                int v = rows[f][x];
                if (v >= sat) ++s.clipped[site][b];
                if (v >= 896) ++s.topHist[site][v - 896];
                if (v > s.maxDn[site]) s.maxDn[site] = v;
            }
            sum[site][b] += bRow[x];
            for (int p = 0; p < P; ++p) {
                double d = static_cast<double>(rows[2 * p][x]) - rows[2 * p + 1][x];
                dsum[site][static_cast<size_t>(b) * P + p] += d;
                d2sum[site][static_cast<size_t>(b) * P + p] += d * d;
                rowD[(static_cast<size_t>(p) * H + y) * 2 + (x & 1)] += d;
                colD[(static_cast<size_t>(p) * 2 + py) * W + x] += d;
            }
        }
    }

    // Pass 2: temporal variance without outliers (blinking / RTS pixels would
    // otherwise dominate dark blocks), spatial variance of the burst mean.
    std::vector<double> mu[4], thr[4], rs[4], rs2[4], rn[4], sv[4];
    for (int k = 0; k < 4; ++k) {
        s.mean[k].assign(nb, 0.0f);
        mu[k].assign(static_cast<size_t>(nb) * P, 0.0);
        thr[k].assign(static_cast<size_t>(nb) * P, 0.0);
        rs[k].assign(static_cast<size_t>(nb) * P, 0.0);
        rs2[k].assign(static_cast<size_t>(nb) * P, 0.0);
        rn[k].assign(static_cast<size_t>(nb) * P, 0.0);
        sv[k].assign(nb, 0.0);
        for (int b = 0; b < nb; ++b) {
            s.mean[k][b] = static_cast<float>(sum[k][b] / (perSite * N));
            for (int p = 0; p < P; ++p) {
                size_t i = static_cast<size_t>(b) * P + p;
                double m = dsum[k][i] / perSite;
                double v = std::max(0.0, d2sum[k][i] / perSite - m * m);
                mu[k][i] = m;
                thr[k][i] = 6.0 * std::sqrt(v) + 1.0;
            }
        }
    }
    for (int y = s.y0; y < gy1; ++y) {
        for (int p = 0; p < P; ++p) {
            for (int f = 2 * p; f < 2 * p + 2; ++f)
                unpackRow(in.frames[f] + static_cast<size_t>(y) * in.rowStride, W, rows[f].data());
        }
        const int by = (y - s.y0) / B, py = y & 1;
        const uint16_t* bRow = burst.data() + static_cast<size_t>(y) * W;
        for (int x = s.x0; x < gx1; ++x) {
            const int site = siteOf(in.cfa, x, py);
            const int b = by * s.blocksX + (x - s.x0) / B;
            double dm = static_cast<double>(bRow[x]) / N - s.mean[site][b];
            sv[site][b] += dm * dm;
            for (int p = 0; p < P; ++p) {
                size_t i = static_cast<size_t>(b) * P + p;
                double d = static_cast<double>(rows[2 * p][x]) - rows[2 * p + 1][x];
                if (std::fabs(d - mu[site][i]) > thr[site][i]) continue;
                rs[site][i] += d;
                rs2[site][i] += d * d;
                rn[site][i] += 1.0;
            }
        }
    }
    double meanAcc[4] = {0, 0, 0, 0}, tvarAcc[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; ++k) {
        s.tvar[k].assign(nb, 0.0f);
        s.svar[k].assign(nb, 0.0f);
        for (int b = 0; b < nb; ++b) {
            double v = 0;
            for (int p = 0; p < P; ++p) {
                size_t i = static_cast<size_t>(b) * P + p;
                double n = std::max(rn[k][i], 1.0);
                double m = rs[k][i] / n;
                v += std::max(0.0, rs2[k][i] / n - m * m) * 0.5; // var(a - b) = 2 var
            }
            s.tvar[k][b] = static_cast<float>(v / P);
            s.svar[k][b] = static_cast<float>(sv[k][b] / perSite);
            meanAcc[k] += s.mean[k][b];
            tvarAcc[k] += s.tvar[k][b];
        }
        s.siteMean[k] = meanAcc[k] / nb;
        s.siteTvar[k] = tvarAcc[k] / nb;
    }

    // Row / column noise: variance of the per-row mean of the pair difference,
    // from adjacent same-site rows so smooth (flicker) variation cancels.
    const int gridW = gx1 - s.x0, gridH = gy1 - s.y0;
    for (int sy = 0; sy < 2; ++sy) {
        for (int sx = 0; sx < 2; ++sx) {
            const int site = siteOf(in.cfa, sx, sy);
            double acc = 0;
            long n = 0;
            for (int p = 0; p < P; ++p) {
                for (int y = s.y0 + sy; y + 2 < gy1; y += 2) {
                    double a = rowD[(static_cast<size_t>(p) * H + y) * 2 + sx] / (gridW / 2);
                    double c = rowD[(static_cast<size_t>(p) * H + y + 2) * 2 + sx] / (gridW / 2);
                    acc += (a - c) * (a - c) * 0.5;
                    ++n;
                }
            }
            s.rowVar[site] = n ? acc / n * 0.5 : 0.0;
            s.rowSamples[site] = gridW / 2;
            acc = 0;
            n = 0;
            for (int p = 0; p < P; ++p) {
                for (int x = s.x0 + sx; x + 2 < gx1; x += 2) {
                    double a = colD[(static_cast<size_t>(p) * 2 + sy) * W + x] / (gridH / 2);
                    double c = colD[(static_cast<size_t>(p) * 2 + sy) * W + x + 2] / (gridH / 2);
                    acc += (a - c) * (a - c) * 0.5;
                    ++n;
                }
            }
            s.colVar[site] = n ? acc / n * 0.5 : 0.0;
            s.colSamples[site] = gridH / 2;
        }
    }

    // Defects on the burst mean: against the median of the 8 same-colour
    // neighbours (2 px apart), threshold from the block's temporal noise.
    if (in.findDefects) {
        std::vector<SensorDefect> found;
        for (int y = 2; y < H - 2; ++y) {
            const int by = std::clamp((y - s.y0) / B, 0, s.blocksY - 1);
            const uint16_t* r0 = burst.data() + static_cast<size_t>(y - 2) * W;
            const uint16_t* r1 = burst.data() + static_cast<size_t>(y) * W;
            const uint16_t* r2 = burst.data() + static_cast<size_t>(y + 2) * W;
            for (int x = 2; x < W - 2; ++x) {
                const int site = siteOf(in.cfa, x, y);
                const int bx = std::clamp((x - s.x0) / B, 0, s.blocksX - 1);
                const float sigma = std::sqrt(s.tvar[site][by * s.blocksX + bx]);
                const double limit = std::max(6.0 * sigma, 4.0) * N; // burst-sum units
                const int c = r1[x];
                int n[8] = {r0[x - 2], r0[x], r0[x + 2], r1[x - 2], r1[x + 2], r2[x - 2], r2[x], r2[x + 2]};
                int s8 = 0;
                for (int v : n) s8 += v;
                if (std::abs(8 * c - s8) < 4.0 * limit) continue; // |c - mean8| < limit / 2
                std::sort(n, n + 8);
                double e = c - 0.5 * (n[3] + n[4]);
                if (std::fabs(e) <= limit) continue;
                ++s.defectCount;
                if (e > 0) ++s.hotCount; else ++s.deadCount;
                if (found.size() < 200000)
                    found.push_back({x, y, site, static_cast<float>(c) / N, static_cast<float>(e / N), sigma});
            }
        }
        size_t keep = std::min(found.size(), static_cast<size_t>(std::max(in.maxDefects, 0)));
        std::partial_sort(found.begin(), found.begin() + keep, found.end(),
                          [](const SensorDefect& a, const SensorDefect& b) { return std::fabs(a.excess) > std::fabs(b.excess); });
        found.resize(keep);
        s.defects = std::move(found);
    }
    s.valid = true;
    return s;
}

std::string sensorStepJson(const SensorStepStats& s) {
    std::string j;
    j.reserve(200000);
    char b[256];
    std::snprintf(b, sizeof(b), "{\"frames\":%d,\"pairs\":%d,\"block\":%d,\"blocksX\":%d,\"blocksY\":%d,\"x0\":%d,\"y0\":%d,",
                  s.frames, s.pairs, s.block, s.blocksX, s.blocksY, s.x0, s.y0);
    j += b;
    appendSites(j, "mean", s.mean, "%.6g");
    j += ',';
    appendSites(j, "tvar", s.tvar, "%.5g");
    j += ',';
    appendSites(j, "svar", s.svar, "%.5g");
    j += ',';
    appendSites(j, "clipped", s.clipped, "%.0f");
    j += ",\"siteMean\":";
    appendArray(j, s.siteMean, 4);
    j += ",\"siteTvar\":";
    appendArray(j, s.siteTvar, 4);
    j += ",\"rowVar\":";
    appendArray(j, s.rowVar, 4);
    j += ",\"colVar\":";
    appendArray(j, s.colVar, 4);
    j += ",\"rowSamples\":";
    appendArray(j, s.rowSamples, 4, "%.0f");
    j += ",\"colSamples\":";
    appendArray(j, s.colSamples, 4, "%.0f");
    j += ",\"maxDn\":";
    appendArray(j, s.maxDn, 4, "%.0f");
    j += ',';
    appendSites(j, "topHist", s.topHist, "%.0f");
    std::snprintf(b, sizeof(b), ",\"defectCount\":%d,\"hotCount\":%d,\"deadCount\":%d,\"defects\":[", s.defectCount,
                  s.hotCount, s.deadCount);
    j += b;
    for (size_t i = 0; i < s.defects.size(); ++i) {
        const SensorDefect& d = s.defects[i];
        std::snprintf(b, sizeof(b), "%s%d,%d,%d,%.2f,%.2f,%.3f", i ? "," : "", d.x, d.y, d.site, d.value, d.excess, d.sigma);
        j += b;
    }
    j += "]}";
    return j;
}

double rawPatchMean(const uint8_t* data, int width, int height, int rowStride, int cfa, const float black[4],
                    float white, double fraction, int siteMask) {
    const int W = width & ~3;
    const int pw = std::max(4, static_cast<int>(W * fraction) & ~3), ph = std::max(2, static_cast<int>(height * fraction) & ~1);
    const int x0 = ((W - pw) / 2) & ~3, y0 = ((height - ph) / 2) & ~1;
    std::vector<uint16_t> row(W);
    double acc = 0;
    long n = 0;
    for (int y = y0; y < y0 + ph; ++y) {
        unpackRow(data + static_cast<size_t>(y) * rowStride, W, row.data());
        for (int x = x0; x < x0 + pw; ++x) {
            int site = siteOf(cfa, x, y);
            if (!(siteMask & (1 << site))) continue;
            acc += (row[x] - black[site]) / (white - black[site]);
            ++n;
        }
    }
    return n ? acc / n : 0.0;
}

namespace {
int rawAt(const uint8_t* src, int rowStride, int x, int y) {
    const uint8_t* g = src + static_cast<size_t>(y) * rowStride + (x >> 2) * 5;
    int i = x & 3;
    return (g[i] << 2) | ((g[4] >> (2 * i)) & 3);
}
} // namespace

void repairRawDefects(const uint8_t* src, uint8_t* dst, int width, int height, int rowStride, const int32_t* xy,
                      size_t count) {
    size_t k = 0;
    while (k < count) {
        const int y = xy[2 * k + 1], gx = xy[2 * k] >> 2;
        uint8_t group[5];
        std::copy(src + static_cast<size_t>(y) * rowStride + gx * 5, src + static_cast<size_t>(y) * rowStride + gx * 5 + 5, group);
        for (; k < count && xy[2 * k + 1] == y && (xy[2 * k] >> 2) == gx; ++k) {
            const int x = xy[2 * k];
            if (x < 2 || y < 2 || x >= width - 2 || y >= height - 2) continue;
            int n[8], c = 0;
            for (int dy = -2; dy <= 2; dy += 2)
                for (int dx = -2; dx <= 2; dx += 2)
                    if (dx || dy) n[c++] = rawAt(src, rowStride, x + dx, y + dy);
            std::sort(n, n + 8);
            const int v = (n[3] + n[4] + 1) >> 1;
            const int i = x & 3;
            group[i] = static_cast<uint8_t>(v >> 2);
            group[4] = static_cast<uint8_t>((group[4] & ~(3 << (2 * i))) | ((v & 3) << (2 * i)));
        }
        std::copy(group, group + 5, dst + static_cast<size_t>(y) * rowStride + gx * 5);
    }
}

std::vector<int32_t> prepareDefects(std::vector<int32_t> xy, int width, int height) {
    std::vector<std::pair<int32_t, int32_t>> p;
    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        int32_t x = xy[i], y = xy[i + 1];
        if (x >= 2 && y >= 2 && x < width - 2 && y < height - 2) p.push_back({y, x});
    }
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end()), p.end());
    std::vector<int32_t> out;
    out.reserve(p.size() * 2);
    for (const auto& [y, x] : p) {
        out.push_back(x);
        out.push_back(y);
    }
    return out;
}

double noiseFactorAt(const std::vector<float>& isos, const std::vector<float>& factors, double iso) {
    const size_t n = std::min(isos.size(), factors.size());
    if (n == 0 || iso <= 0) return 1.0;
    if (iso <= isos[0]) return factors[0];
    if (iso >= isos[n - 1]) return factors[n - 1];
    for (size_t i = 1; i < n; ++i) {
        if (iso > isos[i]) continue;
        double t = std::log(iso / isos[i - 1]) / std::log(static_cast<double>(isos[i]) / isos[i - 1]);
        return std::exp(std::log(factors[i - 1]) + t * (std::log(factors[i]) - std::log(factors[i - 1])));
    }
    return factors[n - 1];
}

} // namespace vesper
