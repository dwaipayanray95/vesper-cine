// Host test for android/app/src/main/cpp/focus_controller.cpp.
#include "focus_controller.h"

#include <cmath>
#include <cstdio>

using namespace vesper;
static int failures = 0;
static void check(bool ok, const char* what, double got) {
    if (!ok) { std::printf("FAIL %s (got %.4f)\n", what, got); ++failures; }
}

int main() {
    // Pull: smooth, eased, speed-limited, arrives.
    FocusController fc;
    fc.setSpeed(1.0);
    fc.pullTo(0.1, 0.7);
    double prev = 0.1, prevV = 0, maxJerk = 0, maxStep = 0;
    int n = 0;
    for (; n < 200 && fc.active(); ++n) {
        double p = fc.update(1.0 / 24);
        double v = p - prev;
        maxJerk = std::fmax(maxJerk, std::fabs(v - prevV));
        maxStep = std::fmax(maxStep, std::fabs(v));
        prevV = v;
        prev = p;
    }
    check(std::fabs(prev - 0.7) < 1e-6, "pull arrives exactly", prev);
    check(maxJerk < 0.01, "pull accelerates smoothly (no jumps)", maxJerk);
    check(maxStep < 1.0 / 24 * 1.01, "pull never exceeds the speed limit", maxStep);
    check(n > 24 * 0.6 && n < 24 * 1.5, "0.6 of full range takes about a second", n);

    // Retarget mid-pull keeps velocity continuous.
    fc.pullTo(0.0, 0.9);
    for (int i = 0; i < 5; ++i) fc.update(1.0 / 24);
    double a = fc.commanded();
    fc.pullTo(0.0, 0.2); // current ignored while moving
    double b = fc.update(1.0 / 24);
    check(b >= a - 1e-9, "retarget decelerates instead of jumping back", b - a);
    std::printf(failures ? "%d FOCUS FAILURES\n" : "all focus controller tests passed\n", failures);
    return failures ? 1 : 0;
}
