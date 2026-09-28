#pragma once

// Cinema-style manual focus pulls, independent of Android so it can be unit tested.
//
// Positions are in "focus units" p = sqrt(diopters / minFocusDiopters) in
// [0, 1] (0 = infinity): equal steps look like equal focus shifts on screen,
// unlike raw diopters which cram everything past ~1 m into a sliver.
//
// Autofocus itself is the HAL's PDAF + laser; Camera2 exposes no distance
// estimate before the lens has moved, so AF moves can't be re-timed here.

namespace vesper {

class FocusController {
public:
    // Seconds for a full-range pull.
    void setSpeed(double fullRangeSeconds) { fullRangeSec_ = fullRangeSeconds > 0.1 ? fullRangeSeconds : 0.1; }

    void pullTo(double current, double target);
    void cancel() { active_ = false; }

    // One tick per camera frame; returns the lens position to command next.
    double update(double dt);

    bool active() const { return active_; }
    double commanded() const { return cmd_; }

private:
    bool active_ = false;
    double fullRangeSec_ = 1.2;
    double cmd_ = 0, target_ = 0, velocity_ = 0;
};

} // namespace vesper
