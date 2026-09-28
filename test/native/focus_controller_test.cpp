// Host test for android/app/src/main/cpp/focus_controller.cpp.
#include "focus_controller.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace vesper;
static int failures = 0;
static void check(bool ok, const char* what, double got) {
    if (!ok) { std::printf("FAIL %s (got %.4f)\n", what, got); ++failures; }
}

// Simulated lens: follows the command with one frame of latency; sharpness is a
// peak at `focus` with a little measurement noise.
static double runSearch(double start, double focus, double* maxStep, int* frames) {
    FocusController fc;
    fc.setSpeed(1.2);
    fc.startSearch(start);
    double lens = start, prevCmd = start;
    *maxStep = 0;
    int n = 0;
    unsigned seed = 7;
    for (; n < 24 * 6 && fc.active(); ++n) {
        seed = seed * 1103515245u + 12345u;
        double noise = ((seed >> 16) % 1000) / 1000.0 * 0.02 - 0.01;
        double sharp = std::exp(-std::pow((lens - focus) / 0.06, 2)) + 0.05 + noise;
        double cmd = fc.update(1.0 / 24, lens, sharp);
        *maxStep = std::fmax(*maxStep, std::fabs(cmd - prevCmd));
        prevCmd = cmd;
        lens = cmd; // arrives by the next frame
    }
    *frames = n;
    return lens;
}

int main() {
    for (double start : {0.0, 0.2, 0.5, 0.9}) {
        for (double focus : {0.05, 0.3, 0.55, 0.8}) {
            double maxStep;
            int frames;
            double end = runSearch(start, focus, &maxStep, &frames);
            if (std::fabs(end - focus) >= 0.03) std::printf("  start %.2f focus %.2f -> %.3f\n", start, focus, end);
            check(std::fabs(end - focus) < 0.03, "search lands on the sharpness peak", end - focus);
            check(maxStep < 1.0 / 1.2 / 24 * 1.01, "lens never moves faster than the speed limit", maxStep);
            check(frames < 24 * 4, "search finishes within 4 s", frames);
        }
    }
    // Pull: smooth, eased, arrives.
    FocusController fc;
    fc.setSpeed(1.0);
    fc.pullTo(0.1, 0.7);
    double prev = 0.1, prevV = 0;
    int n = 0;
    double maxJerk = 0;
    for (; n < 200 && fc.active(); ++n) {
        double p = fc.update(1.0 / 24, prev, 0);
        double v = p - prev;
        maxJerk = std::fmax(maxJerk, std::fabs(v - prevV));
        prevV = v;
        prev = p;
    }
    check(std::fabs(prev - 0.7) < 1e-6, "pull arrives exactly", prev);
    check(maxJerk < 0.01, "pull accelerates smoothly (no jumps)", maxJerk);
    std::printf(failures ? "%d FOCUS FAILURES\n" : "all focus controller tests passed\n", failures);
    return failures ? 1 : 0;
}
