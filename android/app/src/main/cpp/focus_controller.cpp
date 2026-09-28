#include "focus_controller.h"

#include <algorithm>
#include <cmath>

namespace vesper {

namespace {
constexpr double kProbeStep = 0.06;     // focus units moved to learn the direction
constexpr double kPeakDrop = 0.6;       // fallen 40% of the way back to background = past the peak
constexpr int kFallingFrames = 2;       // consecutive frames required (noise guard)
constexpr double kArrive = 0.004;       // close enough to target
constexpr double kPeakContrast = 1.6;   // a real peak is >= 1.6x the flattest (defocused) reading
}

// Velocity-limited move with ease-in/ease-out (acceleration limited, then a
// proportional approach so it lands without a visible stop).
double FocusController::moveToward(double target, double dt, double speedScale) {
    const double vmax = speedScale / fullRangeSec_;           // focus units / s
    const double accel = 1.0 / fullRangeSec_ / 0.25;          // full sweep speed in 0.25 s, same braking everywhere
    const double dist = target - cmd_;
    const double stopDist = velocity_ * velocity_ / (2 * accel);
    double desired = std::copysign(vmax, dist);
    if (std::fabs(dist) <= stopDist + 1e-6) desired = std::copysign(std::sqrt(2 * accel * std::fabs(dist)), dist);
    const double dv = std::clamp(desired - velocity_, -accel * dt, accel * dt);
    velocity_ += dv;
    cmd_ += velocity_ * dt;
    if ((dist > 0 && cmd_ > target) || (dist < 0 && cmd_ < target)) { cmd_ = target; velocity_ = 0; }
    cmd_ = std::clamp(cmd_, 0.0, 1.0);
    return cmd_;
}

void FocusController::pullTo(double current, double target) {
    if (state_ == State::Idle) { cmd_ = current; velocity_ = 0; }
    target_ = std::clamp(target, 0.0, 1.0);
    state_ = State::Pulling;
}

void FocusController::startSearch(double current) {
    cmd_ = current;
    velocity_ = 0;
    probeStart_ = current;
    probeSharp_ = -1;
    probeFrames_ = 0;
    bestSharp_ = -1;
    minSharp_ = 1e30;
    fallingFrames_ = 0;
    reversed_ = false;
    // Probe towards the middle of the range first: more room to discover the slope.
    dir_ = current < 0.5 ? 1 : -1;
    state_ = State::Probing;
}

double FocusController::update(double dt, double measuredP, double sharpness) {
    switch (state_) {
        case State::Idle:
            return cmd_;
        case State::Pulling:
        case State::Settling:
            moveToward(target_, dt, state_ == State::Settling ? 0.6 : 1.0);
            if (std::fabs(cmd_ - target_) < kArrive && std::fabs(velocity_) < 1e-3) { cmd_ = target_; state_ = State::Idle; }
            return cmd_;
        case State::Probing: {
            minSharp_ = std::min(minSharp_, sharpness);
            if (probeSharp_ < 0) probeSharp_ = sharpness;
            if (sharpness > bestSharp_) { bestSharp_ = sharpness; bestP_ = measuredP; }
            ++probeFrames_;
            if (std::fabs(measuredP - probeStart_) >= kProbeStep * 0.8 || probeFrames_ > 20) {
                // Got sharper: keep going. Got softer: turn round (lens position already
                // explored on this side is recorded in best).
                if (sharpness < probeSharp_) dir_ = -dir_;
                state_ = State::Sweeping;
                fallingFrames_ = 0;
            }
            moveToward(std::clamp(probeStart_ + dir_ * kProbeStep, 0.0, 1.0), dt, 0.5);
            return cmd_;
        }
        case State::Sweeping: {
            minSharp_ = std::min(minSharp_, sharpness);
            if (sharpness > bestSharp_) { bestSharp_ = sharpness; bestP_ = measuredP; fallingFrames_ = 0; }
            // After a wrong-way probe the lens first re-crosses ground it has
            // already measured; only judge "past the peak" on new ground.
            else if ((measuredP - probeStart_) * dir_ > 0 &&
                     sharpness < minSharp_ + (bestSharp_ - minSharp_) * kPeakDrop) ++fallingFrames_;
            // Only a peak that stands clearly above the defocused background
            // counts; on a flat, noisy curve keep sweeping.
            const bool realPeak = bestSharp_ > minSharp_ * kPeakContrast;
            const double end = dir_ > 0 ? 1.0 : 0.0;
            const bool atEnd = std::fabs(cmd_ - end) < kArrive;
            if (atEnd && !realPeak && !reversed_) {
                dir_ = -dir_;
                reversed_ = true;
                velocity_ = 0;
                return cmd_;
            }
            if ((fallingFrames_ >= kFallingFrames && realPeak) || atEnd) {
                target_ = bestP_;
                state_ = State::Settling;
                return cmd_;
            }
            moveToward(end, dt, 1.0);
            return cmd_;
        }
    }
    return cmd_;
}

} // namespace vesper
