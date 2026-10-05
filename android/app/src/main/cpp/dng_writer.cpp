#include "dng_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vesper {
namespace {

enum : uint16_t { kByte = 1, kAscii = 2, kShort = 3, kLong = 4, kRational = 5, kUndefined = 7, kSRational = 10, kDouble = 12 };

struct Entry {
    uint16_t tag, type;
    uint32_t count;
    std::vector<uint8_t> bytes; // little endian, except OpcodeList (big endian by spec)
};

int siteOf(int cfa, int px, int py) {
    int bit = ((py & 1) << 1) | (px & 1);
    switch (cfa) {
        case 0: return bit;
        case 1: return bit ^ 1;
        case 2: return bit ^ 2;
        default: return 3 - bit;
    }
}

void putLE(std::vector<uint8_t>& v, uint64_t x, int n) {
    for (int i = 0; i < n; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void putBE(std::vector<uint8_t>& v, uint64_t x, int n) {
    for (int i = n - 1; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void putBEf(std::vector<uint8_t>& v, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    putBE(v, u, 4);
}
void putBEd(std::vector<uint8_t>& v, double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    putBE(v, u, 8);
}

struct Ifd {
    std::vector<Entry> e;
    void shorts(uint16_t tag, std::initializer_list<uint32_t> vals) {
        Entry x{tag, kShort, static_cast<uint32_t>(vals.size()), {}};
        for (uint32_t v : vals) putLE(x.bytes, v, 2);
        e.push_back(std::move(x));
    }
    void longs(uint16_t tag, std::initializer_list<uint32_t> vals) {
        Entry x{tag, kLong, static_cast<uint32_t>(vals.size()), {}};
        for (uint32_t v : vals) putLE(x.bytes, v, 4);
        e.push_back(std::move(x));
    }
    void bytes(uint16_t tag, uint16_t type, const std::vector<uint8_t>& b) {
        e.push_back({tag, type, static_cast<uint32_t>(b.size()), b});
    }
    void ascii(uint16_t tag, const std::string& s) {
        std::vector<uint8_t> b(s.begin(), s.end());
        b.push_back(0);
        bytes(tag, kAscii, b);
    }
    void rationals(uint16_t tag, const double* v, int n, uint32_t den = 10000) {
        Entry x{tag, kRational, static_cast<uint32_t>(n), {}};
        for (int i = 0; i < n; ++i) {
            putLE(x.bytes, static_cast<uint32_t>(std::lround(std::max(0.0, v[i]) * den)), 4);
            putLE(x.bytes, den, 4);
        }
        e.push_back(std::move(x));
    }
    void srationals(uint16_t tag, const float* v, int n) {
        Entry x{tag, kSRational, static_cast<uint32_t>(n), {}};
        for (int i = 0; i < n; ++i) {
            putLE(x.bytes, static_cast<uint32_t>(static_cast<int32_t>(std::lround(v[i] * 10000.0))), 4);
            putLE(x.bytes, 10000u, 4);
        }
        e.push_back(std::move(x));
    }
    void doubles(uint16_t tag, const double* v, int n) {
        Entry x{tag, kDouble, static_cast<uint32_t>(n), {}};
        for (int i = 0; i < n; ++i) {
            uint64_t u;
            std::memcpy(&u, &v[i], 8);
            putLE(x.bytes, u, 8);
        }
        e.push_back(std::move(x));
    }
};

// GainMap opcodes (DNG 1.3 opcode 9), one per CFA position, from the Camera2
// lens-shading map. The map spans the pre-correction array edge to edge; map
// coordinates are relative to this image (0..1), so a cropped readout (16:9)
// gets an origin outside 0..1.
std::vector<uint8_t> gainMapOpcodes(const DngImageInfo& in) {
    std::vector<uint8_t> o;
    const int cols = in.shadingCols, rows = in.shadingRows;
    putBE(o, 4, 4);
    const double sx = in.sensorMap[0], sy = in.sensorMap[1];
    const double spacingH = in.arrayWidth / (cols - 1) / (sx * in.width);
    const double spacingV = in.arrayHeight / (rows - 1) / (sy * in.height);
    const double originH = -in.sensorMap[2] / (sx * in.width);
    const double originV = -in.sensorMap[3] / (sy * in.height);
    for (int py = 0; py < 2; ++py) {
        for (int px = 0; px < 2; ++px) {
            int site = siteOf(in.cfa, px, py);
            int channel = site == 0 ? 0 : site == 3 ? 3 : (py == 0 ? 1 : 2); // greens by row parity
            putBE(o, 9, 4);           // GainMap
            putBE(o, 0x01030000, 4);  // DNG 1.3
            putBE(o, 0, 4);           // not optional
            putBE(o, 76 + 4u * cols * rows, 4);
            putBE(o, py, 4);          // Top
            putBE(o, px, 4);          // Left
            putBE(o, in.height, 4);   // Bottom
            putBE(o, in.width, 4);    // Right
            putBE(o, 0, 4);           // Plane
            putBE(o, 1, 4);           // Planes
            putBE(o, 2, 4);           // RowPitch
            putBE(o, 2, 4);           // ColPitch
            putBE(o, rows, 4);
            putBE(o, cols, 4);
            putBEd(o, spacingV);
            putBEd(o, spacingH);
            putBEd(o, originV);
            putBEd(o, originH);
            putBE(o, 1, 4);           // MapPlanes
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c) putBEf(o, in.shading[(r * cols + c) * 4 + channel]);
        }
    }
    return o;
}

} // namespace

bool writeDng(const std::string& path, const uint8_t* raw10, const DngImageInfo& in, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const int W = in.width & ~3, H = in.height;
    if (!raw10 || W <= 0 || H <= 0 || in.rowStride < W * 5 / 4) return fail("bad image size");

    Ifd ifd;
    ifd.longs(254, {0});                       // NewSubFileType: main image
    ifd.longs(256, {static_cast<uint32_t>(W)});
    ifd.longs(257, {static_cast<uint32_t>(H)});
    ifd.shorts(258, {16});
    ifd.shorts(259, {1});                      // uncompressed
    ifd.shorts(262, {32803});                  // CFA
    ifd.ascii(271, in.make);
    ifd.ascii(272, in.model);
    ifd.longs(273, {0});                       // StripOffsets, patched below
    ifd.shorts(274, {static_cast<uint32_t>(in.orientation)});
    ifd.shorts(277, {1});
    ifd.longs(278, {static_cast<uint32_t>(H)});
    ifd.longs(279, {static_cast<uint32_t>(W) * H * 2});
    ifd.shorts(284, {1});
    ifd.ascii(305, in.software);
    if (in.dateTime.size() == 19) ifd.ascii(306, in.dateTime);
    ifd.shorts(33421, {2, 2});
    {
        std::vector<uint8_t> pat;
        for (int py = 0; py < 2; ++py)
            for (int px = 0; px < 2; ++px) {
                int site = siteOf(in.cfa, px, py);
                pat.push_back(site == 0 ? 0 : site == 3 ? 2 : 1);
            }
        ifd.bytes(33422, kByte, pat);
    }
    if (in.exposureNs > 0) {
        Entry x{33434, kRational, 1, {}};
        putLE(x.bytes, static_cast<uint32_t>(std::min<int64_t>(in.exposureNs / 1000, 0xFFFFFFFFll)), 4);
        putLE(x.bytes, 1000000u, 4);
        ifd.e.push_back(std::move(x));
    }
    if (in.fNumber > 0) {
        double f = in.fNumber;
        ifd.rationals(33437, &f, 1, 100);
    }
    if (in.iso > 0) ifd.shorts(34855, {static_cast<uint32_t>(std::min(in.iso, 65535))});
    if (in.focalLength > 0) {
        double f = in.focalLength;
        ifd.rationals(37386, &f, 1, 1000);
    }
    ifd.bytes(50706, kByte, {1, 4, 0, 0});     // DNGVersion
    ifd.bytes(50707, kByte, {1, 3, 0, 0});     // DNGBackwardVersion (GainMap)
    ifd.ascii(50708, in.uniqueModel.empty() ? in.make + " " + in.model : in.uniqueModel);
    ifd.shorts(50713, {2, 2});                 // BlackLevelRepeatDim
    {
        double bl[4];
        for (int py = 0; py < 2; ++py)
            for (int px = 0; px < 2; ++px) bl[py * 2 + px] = in.black[siteOf(in.cfa, px, py)];
        ifd.rationals(50714, bl, 4, 100);
    }
    ifd.longs(50717, {static_cast<uint32_t>(std::lround(in.white))});
    ifd.srationals(50721, in.colorMatrix1.data(), 9);
    if (in.haveColorMatrix2) ifd.srationals(50722, in.colorMatrix2.data(), 9);
    ifd.srationals(50723, in.calibration1.data(), 9);
    if (in.haveColorMatrix2) ifd.srationals(50724, in.calibration2.data(), 9);
    {
        double n[3] = {in.asShotNeutral[0], in.asShotNeutral[1], in.asShotNeutral[2]};
        ifd.rationals(50728, n, 3, 1000000);
    }
    ifd.shorts(50778, {static_cast<uint32_t>(in.illuminant1)});
    if (in.haveColorMatrix2) ifd.shorts(50779, {static_cast<uint32_t>(in.illuminant2)});
    if (in.haveForwardMatrix1) ifd.srationals(50964, in.forwardMatrix1.data(), 9);
    if (in.haveForwardMatrix2 && in.haveColorMatrix2) ifd.srationals(50965, in.forwardMatrix2.data(), 9);
    if (in.shading && in.shadingCols >= 2 && in.shadingRows >= 2 && in.arrayWidth > 0 && in.arrayHeight > 0)
        ifd.bytes(51009, kUndefined, gainMapOpcodes(in)); // OpcodeList2: after black subtraction, before demosaic
    if (in.noiseChannels > 0) {
        // Per colour plane (R, G, B), as Android's DngCreator: the CFA channel
        // of that colour with the largest S.
        double np[6] = {0, 0, 0, 0, 0, 0};
        for (int plane = 0; plane < 3; ++plane) {
            bool set = false;
            for (int i = 0; i < 4; ++i) {
                int ch = in.noiseChannels >= 4 ? i : 0;
                int site = siteOf(in.cfa, i & 1, i >> 1);
                int color = site == 0 ? 0 : site == 3 ? 2 : 1;
                if (color != plane) continue;
                if (!set || in.noiseProfile[ch * 2] > np[plane * 2]) {
                    np[plane * 2] = in.noiseProfile[ch * 2];
                    np[plane * 2 + 1] = in.noiseProfile[ch * 2 + 1];
                    set = true;
                }
            }
        }
        ifd.doubles(51041, np, 6);
    }
    std::sort(ifd.e.begin(), ifd.e.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

    // Layout: header, IFD0, out-of-line values, image strip.
    const uint32_t ifdOffset = 8, ifdSize = 2 + 12 * static_cast<uint32_t>(ifd.e.size()) + 4;
    uint32_t dataOffset = ifdOffset + ifdSize;
    std::vector<uint32_t> valueOffsets(ifd.e.size(), 0);
    for (size_t i = 0; i < ifd.e.size(); ++i) {
        if (ifd.e[i].bytes.size() <= 4) continue;
        valueOffsets[i] = dataOffset;
        dataOffset += static_cast<uint32_t>((ifd.e[i].bytes.size() + 1) & ~size_t(1));
    }
    const uint32_t imageOffset = (dataOffset + 15) & ~15u;
    for (auto& x : ifd.e)
        if (x.tag == 273) {
            x.bytes.clear();
            putLE(x.bytes, imageOffset, 4);
        }

    std::vector<uint8_t> head;
    head.reserve(imageOffset);
    head.insert(head.end(), {'I', 'I', 42, 0});
    putLE(head, ifdOffset, 4);
    putLE(head, ifd.e.size(), 2);
    for (size_t i = 0; i < ifd.e.size(); ++i) {
        const Entry& x = ifd.e[i];
        putLE(head, x.tag, 2);
        putLE(head, x.type, 2);
        putLE(head, x.count, 4);
        if (x.bytes.size() <= 4) {
            std::vector<uint8_t> v = x.bytes;
            v.resize(4, 0);
            head.insert(head.end(), v.begin(), v.end());
        } else {
            putLE(head, valueOffsets[i], 4);
        }
    }
    putLE(head, 0, 4); // no next IFD
    for (const auto& x : ifd.e) {
        if (x.bytes.size() <= 4) continue;
        head.insert(head.end(), x.bytes.begin(), x.bytes.end());
        if (x.bytes.size() & 1) head.push_back(0);
    }
    head.resize(imageOffset, 0);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return fail("cannot open file");
    bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size();
    std::vector<uint8_t> row(static_cast<size_t>(W) * 2);
    for (int y = 0; ok && y < H; ++y) {
        const uint8_t* src = raw10 + static_cast<size_t>(y) * in.rowStride;
        for (int g = 0; g < W / 4; ++g) {
            const uint8_t* p = src + g * 5;
            for (int i = 0; i < 4; ++i) {
                uint16_t v = static_cast<uint16_t>((p[i] << 2) | ((p[4] >> (2 * i)) & 3));
                row[(g * 4 + i) * 2] = static_cast<uint8_t>(v);
                row[(g * 4 + i) * 2 + 1] = static_cast<uint8_t>(v >> 8);
            }
        }
        ok = std::fwrite(row.data(), 1, row.size(), f) == row.size();
    }
    if (std::fclose(f) != 0) ok = false;
    return ok ? true : fail("write failed");
}

} // namespace vesper
