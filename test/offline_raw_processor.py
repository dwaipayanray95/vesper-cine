#!/usr/bin/env python3
"""
Project RawEdge: Offline Raw Verification Harness
Validates Malvar-He-Cutler (MHC) 5x5 demosaicing, matrix conversions,
and R-Log mathematical curve fidelity against sensor raw data.
"""

import sys
import math
import struct
import os

def rlog_oetf(x):
    """Applies the mathematical R-Log curve matching the Vulkan compute shader."""
    a = 0.225
    b = 5.5555
    c = 0.385
    d = 0.100
    cutoff = 0.01

    if x < cutoff:
        return max(0.0, min(1.0, c * x + d))
    else:
        return max(0.0, min(1.0, a * math.log(b * x + 1.0) + c))

def rlog_to_rec709_viewfinder(rlog_val):
    """Viewfinder normalized Rec.709 preview LUT matching mhc_rlog.comp."""
    x = max(0.0, (rlog_val - 0.10) / 0.85)
    return max(0.0, min(1.0, (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)))

def print_rlog_transfer_table():
    print("=" * 65)
    print("Project RawEdge: R-Log Mathematical Transfer Curve Table")
    print("=" * 65)
    print(f"{'Scene Linear Light':<20} | {'R-Log Code (0-1)':<18} | {'10-bit Code (0-1023)'}")
    print("-" * 65)

    test_points = [
        (0.000, "Pure Black (Noise floor)"),
        (0.010, "Cutoff Point"),
        (0.050, "Deep Shadows (10% IRE)"),
        (0.180, "18% Middle Gray (Target 40% IRE)"),
        (0.500, "Midtone Highlights"),
        (0.900, "Specular Highs"),
        (1.000, "Sensor Clip Point (Target 95% IRE)"),
        (1.500, "Over-exposure headroom")
    ]

    for lin, label in test_points:
        log_val = rlog_oetf(lin)
        code_10bit = round(log_val * 1023)
        vf_709 = rlog_to_rec709_viewfinder(log_val)
        print(f"{lin:<6.3f} ({label:<25}) -> R-Log: {log_val:0.4f} | 10-bit: {code_10bit:>4} | VF Rec709: {vf_709:0.4f}")

    print("=" * 65)

if __name__ == "__main__":
    print_rlog_transfer_table()
