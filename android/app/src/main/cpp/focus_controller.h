#pragma once

// Cinema-style focus motion, independent of Android so it can be unit tested.
//
// Positions are in "focus units" p = sqrt(diopters / minFocusDiopters) in
// [0, 1] (0 = infinity): equal steps look like equal focus shifts on screen,
// unlike raw diopters which cram everything past ~1 m into a sliver.
//
//  * pullTo(): a smooth rack to a known position (slider, focus marks, lock).
//  * startSearch(): contrast-detect AF. The HAL's PDAF drive speed can't be
//    controlled through Camera2 (it snaps), so for gradual "cine" focus we
//    move the lens ourselves at a limited speed, measure sharpness at the
//    tapped region every frame, pass the peak slightly, then ease back onto it.

namespace vesper {

class FocusController {
public:
    enum class State { Idle, Pulling, Probing, Sweeping, Settling };

    // Seconds for a full-range pull; searches sweep at a similar pace.
    void setSpeed(double fullRangeSeconds) { fullRangeSec_ = fullRangeSeconds > 0.1 ? fullRangeSeconds : 0.1; }

    void pullTo(double current, double target);
    void startSearch(double current);
    void cancel() { state_ = State::Idle; }

    // One tick per camera frame. `measuredP` is the lens position the frame
    // was actually exposed at (from CaptureResult) and `sharpness` the focus
    // metric of the AF region in that frame (ignored unless searching).
    // Returns the lens position to command next.
    double update(double dt, double measuredP, double sharpness);

    State state() const { return state_; }
    bool active() const { return state_ != State::Idle; }
    double commanded() const { return cmd_; }

private:
    double moveToward(double target, double dt, double speedScale);

    State state_ = State::Idle;
    double fullRangeSec_ = 1.2;
    double cmd_ = 0, target_ = 0, velocity_ = 0;
    double dir_ = 1;
    double probeStart_ = 0, probeSharp_ = 0;
    double bestP_ = 0, bestSharp_ = 0;
    int fallingFrames_ = 0;
    int probeFrames_ = 0;
    double minSharp_ = 0;
    bool reversed_ = false;
};

} // namespace vesper
