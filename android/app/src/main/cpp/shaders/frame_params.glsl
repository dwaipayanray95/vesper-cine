// Shared per-frame parameters (std140). Mirrors vesper::FrameParams in
// vulkan_engine.h — every member is a 16-byte vec4/ivec4 so the C++ and GLSL
// layouts can't drift apart through alignment rules.
layout(std140, set = 0, binding = 0) uniform FrameParams {
    vec4  blackLevel;   // raw DN per logical CFA channel: R, Gr, Gb, B
    vec4  wbGains;      // r, g, b camera-space gains (g = 1); w = white level (DN)
    ivec4 rawInfo;      // raw width, raw height, row stride (bytes), CFA pattern (0=RGGB 1=GRBG 2=GBRG 3=BGGR)
    ivec4 quadInfo;     // quad width, quad height, shading map cols, rows (0 = no map)
    vec4  cropRect;     // crop in raw pixel coordinates: x0, y0, width, height
    ivec4 outInfo;      // output width, height, rotation (0/90/180/270 cw), monitoring mode
    ivec4 flags;        // x: write P010, y: swap R/B in viewfinder, z: write viewfinder
    vec4  cfaOffsets;   // raw-space centre of the R sample (xy) and B sample (zw) inside a quad
    vec4  exposure;     // x: clip linear k, y: zebra threshold (fraction of clip), z: peaking threshold, w: log2(k / 0.18)
    vec4  camToRec2020[3]; // rows of the 3x3 matrix (white-balanced camera RGB -> scene-linear Rec.2020, includes k)
} P;
