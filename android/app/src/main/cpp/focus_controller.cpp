#include "focus_controller.h"

#include <algorithm>
#include <cmath>

namespace vesper {

namespace {
constexpr double kArrive = 0.004; // close enough to target
}

void FocusController::pullTo(double current, double target) {
    if (!active_) { cmd_ = current; velocity_ = 0; }
    target_ = std::clamp(target, 0.0, 1.0);
    active_ = true;
}

// Velocity-limited move with ease-in/ease-out (acceleration limited, then a
// square-root braking curve so it lands without a visible stop).
double FocusController::update(double dt) {
    if (!active_) return cmd_;
    const double vmax = 1.0 / fullRangeSec_;        // focus units / s
    const double accel = 1.0 / fullRangeSec_ / 0.25; // full speed in 0.25 s
    const double dist = target_ - cmd_;
    const double stopDist = velocity_ * velocity_ / (2 * accel);
    double desired = std::copysign(vmax, dist);
    if (std::fabs(dist) <= stopDist + 1e-6) desired = std::copysign(std::sqrt(2 * accel * std::fabs(dist)), dist);
    velocity_ += std::clamp(desired - velocity_, -accel * dt, accel * dt);
    cmd_ += velocity_ * dt;
    if ((dist > 0 && cmd_ > target_) || (dist < 0 && cmd_ < target_)) { cmd_ = target_; velocity_ = 0; }
    cmd_ = std::clamp(cmd_, 0.0, 1.0);
    if (std::fabs(cmd_ - target_) < kArrive && std::fabs(velocity_) < 1e-3) { cmd_ = target_; active_ = false; }
    return cmd_;
}

void ExposureRamp::start(double t0, double iso0, double t1, double iso1) {
    lt0_ = std::log2(t0); li0_ = std::log2(iso0);
    lt1_ = std::log2(t1); li1_ = std::log2(iso1);
    const double stops = std::fabs((lt1_ + li1_) - (lt0_ + li0_));
    dur_ = std::clamp(stops * 0.5, 0.3, 1.5);
    elapsed_ = 0;
    active_ = true;
}

bool ExposureRamp::update(double dt, double& t, double& iso) {
    elapsed_ += dt;
    double x = std::clamp(elapsed_ / dur_, 0.0, 1.0);
    double k = x * x * (3 - 2 * x); // smoothstep
    t = std::exp2(lt0_ + (lt1_ - lt0_) * k);
    iso = std::exp2(li0_ + (li1_ - li0_) * k);
    if (x >= 1) active_ = false;
    return active_;
}

} // namespace vesper
