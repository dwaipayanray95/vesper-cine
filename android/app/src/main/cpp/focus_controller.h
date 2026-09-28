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

// Smooth exposure transition (shutter + ISO) in log space with ease-in/out,
// so auto-exposure glides instead of jumping. Duration scales with the size of
// the change: ~0.5 s per stop, 0.3-1.5 s.
class ExposureRamp {
public:
    void start(double t0, double iso0, double t1, double iso1);
    // Returns false once finished (t/iso then hold the target).
    bool update(double dt, double& t, double& iso);
    bool active() const { return active_; }
    void cancel() { active_ = false; }
    double duration() const { return dur_; }

private:
    bool active_ = false;
    double lt0_ = 0, li0_ = 0, lt1_ = 0, li1_ = 0, elapsed_ = 0, dur_ = 0;
};

} // namespace vesper
