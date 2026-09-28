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

} // namespace vesper
