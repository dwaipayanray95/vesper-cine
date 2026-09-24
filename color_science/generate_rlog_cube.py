#!/usr/bin/env python3
"""
Generates standard 3D .cube conversion LUT for Project RawEdge:
Transforms R-Log (Rec.2020) to high-fidelity Rec.709 with filmic highlight roll-off.
Compatible with DaVinci Resolve, Premiere Pro, Final Cut Pro, and CapCut.
"""

import math

LUT_SIZE = 33

def rlog_to_linear(x):
    a = 0.225
    b = 5.5555
    c = 0.385
    d = 0.100
    cutoff_log = 0.10385

    if x < cutoff_log:
        return max(0.0, (x - d) / c)
    else:
        return max(0.0, (math.exp((x - c) / a) - 1.0) / b)

def rec2020_to_rec709(r, g, b):
    # Rec.2020 to Rec.709 color matrix
    r_709 =  1.6605 * r - 0.1246 * g - 0.0182 * b
    g_709 = -0.5876 * r + 1.1329 * g - 0.1006 * b
    b_709 = -0.0728 * r - 0.0083 * g + 1.1187 * b
    return r_709, g_709, b_709

def filmic_tonemap(x):
    # ACES-style filmic tone-mapping curve
    x = max(0.0, x)
    a = 2.51
    b = 0.03
    c = 2.43
    d = 0.59
    e = 0.14
    return min(1.0, max(0.0, (x * (a * x + b)) / (x * (c * x + d) + e)))

def generate_cube(output_filename="R-Log_to_Rec709.cube"):
    with open(output_filename, "w") as f:
        f.write("# Project RawEdge - R-Log to Rec.709 Technical LUT\n")
        f.write("# Calibrated for Google Pixel Sensor Linearization\n")
        f.write(f"LUT_3D_SIZE {LUT_SIZE}\n\n")

        for b_idx in range(LUT_SIZE):
            b_in = b_idx / (LUT_SIZE - 1)
            b_lin = rlog_to_linear(b_in)

            for g_idx in range(LUT_SIZE):
                g_in = g_idx / (LUT_SIZE - 1)
                g_lin = rlog_to_linear(g_in)

                for r_idx in range(LUT_SIZE):
                    r_in = r_idx / (LUT_SIZE - 1)
                    r_lin = rlog_to_linear(r_in)

                    # Matrix to Rec.709
                    r_709, g_709, b_709 = rec2020_to_rec709(r_lin, g_lin, b_lin)

                    # Filmic tonemapping
                    r_out = filmic_tonemap(r_709)
                    g_out = filmic_tonemap(g_709)
                    b_out = filmic_tonemap(b_709)

                    f.write(f"{r_out:.6f} {g_out:.6f} {b_out:.6f}\n")

    print(f"Generated {output_filename} successfully with grid size {LUT_SIZE}^3")

if __name__ == "__main__":
    generate_cube("color_science/R-Log_to_Rec709.cube")
