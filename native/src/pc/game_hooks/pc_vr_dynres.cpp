// The dynamic resolution in the headset (xr.ini dynres, the options menu's Realidad virtual >
// "Resolución dinámica"; Android only, off by default): the internal resolution of the mode in effect
// (xr.ini's resolution / stereo_resolution / window_resolution, the window's fewer lines as it grows)
// goes down one step when the game's frame rate stays low and up one when it holds, through the same
// path as a mode change (pc_menu.cpp syncVrResolution -> setVrScale -> VISetFrameBufferScale).
//
// - The rate: the lower of the game's frames (over the wall time between pc_menu.cpp's frames, one per
//   game frame) and the frames the headset showed (wq_xr_frames_shown: Aurora's frames the XR layer
//   copied once its GPU had finished them), over windows of 1 s. Not the compositor's 72/90 Hz: it
//   reprojects whatever the game hands it. 1.1.1: the game's own rate alone stayed at 30 under a
//   GPU-bound scene (the game thread records ahead; the GPU finished a frame in three, the rest
//   replaced before shown), so dynres went back up while the headset showed 10 fps. The shown rate
//   with a tolerance (10% and a frame: the window's edge, the 60 fps mode's beat against 72 Hz); going
//   up also needs the headset to show 80% of the game's frames. A window with nothing shown (the
//   headset not rendering) is not counted.
// - Down one step (a tenth of the mode's lines) after 3 windows in a row under dynres_low_fps (27),
//   two steps when the rate is under 60% of it; up one after 10 windows in a row at dynres_high_fps
//   (29.5) or more. Between the two, nothing.
// - A step up that drops again within 20 s doubles the next wait to go up (10, 20, 40... 120 s); 60 s
//   at a level without a step down resets it. At 30 fps the game paces itself, so its rate cannot
//   show headroom: going up is a probe, and this keeps it from flipping every few seconds.
// - A step down that did not raise the rate by 1 fps while the GPU was not busy (under 90%, or no
//   reading: the CPU is the limit, many characters, Windfall) holds further steps down for 30 s: lower
//   lines would not help. With the GPU at its limit it goes on down to dynres_min.
// - Never under dynres_min (0.7) of the mode's lines, nor over them: xr.ini's resolutions are the top.
// - The second step after the vegetation's adaptive distance (pc_veg_distance.cpp): with it on, a step
//   down only once it is at vegetation_min and the GPU still does not keep up (fewer plants first).
// - Each step resizes the EFB (Aurora recreates its render targets and clears the copy caches: a
//   hitch of a frame or two), so the 2 s after a step, frames over 250 ms (loading, a pipeline built),
//   the options menu, the headset's panel and a session not shown are not counted.
// - A change of the mode's key or of its value (the mode, the 3D, the Resolución row, a preset) starts
//   over at the top. settings.ini's Resolución interna (COS_FB_SCALE) in effect at the start: idle
//   until the headset's resolution is applied once (the caller's `idle`).
#include "pc/pc_vr.h"
#include "pc_internal.h"

#include <algorithm>
#include <cmath>
#include <unistd.h>

#if defined(__ANDROID__)
#include "wq_xr.h"
#endif

#if defined(__ANDROID__)
extern "C" int pc_fps60_test(void); // pc_frame.cpp: the 60 fps mode is on

namespace {

constexpr float kFactors[] = {1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f};
constexpr int kLevels = (int)(sizeof(kFactors) / sizeof(kFactors[0]));
constexpr uint64_t kSecondNs = 1000000000ull;
constexpr uint64_t kLongFrameNs = 250000000ull;  // a frame this long is a load or a hitch: not counted
constexpr unsigned kDownWindows = 3;
constexpr unsigned kUpWindows = 10;
constexpr unsigned kUpWindowsMax = 120;
constexpr uint64_t kSettleNs = 2 * kSecondNs;         // after a step: the resize's hitch
constexpr uint64_t kUnstableNs = 20 * kSecondNs;      // a step down this soon after a step up
constexpr uint64_t kStableNs = 60 * kSecondNs;        // a level held this long: the wait back to 10 s
constexpr uint64_t kNoGainHoldNs = 30 * kSecondNs;    // no gain from a step down: no more for this long

struct State {
    bool on = false;
    int level = 0;
    int key = 0;          // the mode's base lines when the level was set (a change starts over)
    uint64_t lastNs = 0;  // the previous frame (0: none counted)
    uint64_t winNs = 0;
    unsigned winFrames = 0;
    uint64_t winStartNs = 0;      // the window's wall start, and the headset's frames shown then
    uint32_t winShownStart = 0;
    unsigned low = 0, high = 0;  // windows in a row under / over the thresholds
    unsigned upWait = kUpWindows;
    uint64_t changeNs = 0;       // the last step
    uint64_t upNs = 0;           // the last step up
    uint64_t holdDownUntil = 0;  // no step down before this (no gain)
    float fpsBeforeDown = 0.0f;  // the rate that made the last step down (0: checked)
    float lastFps = 0.0f;
} s;

void resetCount() {
    s.lastNs = 0;
    s.winNs = 0;
    s.winFrames = 0;
    s.winStartNs = 0;
    s.low = s.high = 0;
}

int linesAt(int lines, int level) {
    if (level == 0) return lines;  // the mode's own, as without dynres
    // even lines, never under 360 (COS_FB_SCALE 0.75)
    const int v = (int)std::lround(lines * kFactors[level] / 2.0f) * 2;
    return v < 360 ? 360 : v;
}

} // namespace
#endif

int pc_vr_dynres_lines(int lines, int base, bool idle) {
#if defined(__ANDROID__)
    WqXrTable table;
    wq_xr_table(&table);
    if (table.dynres != s.on) {
        s.on = table.dynres;
        pc::writef(STDERR_FILENO, "[vr] dynres: %s (the frames the headset shows: down under %.1f fps for 3 s, up "
                   "at %.1f for 10 s, down to %.2f)\n",
                   s.on ? "on" : "off", table.dynresLowFps, table.dynresHighFps, table.dynresMin);
        s.level = 0;
        s.upWait = kUpWindows;
        resetCount();
    }
    if (!s.on || lines <= 0) {
        return lines;
    }
    if (base != s.key) {
        // the mode, the 3D, the row or a preset: its own resolution again, from the top
        if (s.level != 0 && s.key != 0) {
            pc::writef(STDERR_FILENO, "[vr] dynres: the mode's resolution changed (%d): back to the top\n", base);
        }
        s.key = base;
        s.level = 0;
        s.upWait = kUpWindows;
        s.fpsBeforeDown = 0.0f;
        resetCount();
    }
    int floorLevel = 0;  // the lowest level dynres_min allows
    while (floorLevel + 1 < kLevels && kFactors[floorLevel + 1] >= table.dynresMin - 0.001f) floorLevel++;
    if (s.level > floorLevel) s.level = floorLevel;  // dynres_min raised meanwhile

    WqXrHead head;
    if (idle || !wq_xr_head(&head)) {
        resetCount();
        return linesAt(lines, s.level);
    }
    const uint64_t now = pc::monotonicNs();
    const uint64_t dt = s.lastNs != 0 ? now - s.lastNs : 0;
    s.lastNs = now;
    if (dt == 0 || now - s.changeNs < kSettleNs) {
        return linesAt(lines, s.level);
    }
    if (dt > kLongFrameNs) {
        // a load or a hitch: the window starts over (the counts in a row stay)
        s.winNs = 0;
        s.winFrames = 0;
        s.winStartNs = 0;
        return linesAt(lines, s.level);
    }
    if (s.winStartNs == 0) {
        s.winStartNs = now;
        s.winShownStart = wq_xr_frames_shown();
    }
    s.winNs += dt;
    s.winFrames++;
    if (s.winNs < kSecondNs) {
        return linesAt(lines, s.level);
    }
    const float gameFps = (float)(s.winFrames * 1e9 / (double)s.winNs);
    const uint32_t shownNow = wq_xr_frames_shown();
    const bool noneShown = shownNow == s.winShownStart;
    const float shownFps = now > s.winStartNs ? (float)((shownNow - s.winShownStart) * 1e9 / (double)(now - s.winStartNs))
                                              : gameFps;
    s.winNs = 0;
    s.winFrames = 0;
    s.winStartNs = 0;
    if (noneShown) {
        // nothing shown for a second while the game runs: the headset is not rendering (taken off, the
        // system's menu over it), not a slow GPU; the window is not counted
        s.low = s.high = 0;
        return linesAt(lines, s.level);
    }
    // The shown rate with a tolerance: a frame of the window's edge, and a tenth for the 60 fps mode's
    // beat against the headset's 72 Hz (a frame now and then replaced before the layer takes it). A
    // GPU-bound scene shows a third to a half of the game's frames, far under it either way.
    const float shownTol = shownFps * 1.1f + 1.0f;
    // down on the lower of the two; up on the game's rate, with the headset showing most of its frames
    const float fps = std::min(gameFps, shownTol);
    const bool showsMost = shownFps >= 0.8f * gameFps;
    s.lastFps = fps;
    const float busy = wq_xr_gpu_busy();  // percent, -1 unknown
    const float scale = pc_fps60_test() != 0 ? 2.0f : 1.0f;
    const float lowFps = table.dynresLowFps * scale, highFps = table.dynresHighFps * scale;

    // the first window after a step down: did it help?
    if (s.fpsBeforeDown > 0.0f) {
        if (fps < s.fpsBeforeDown + 1.0f && fps < lowFps && busy < 90.0f) {
            s.holdDownUntil = now + kNoGainHoldNs;
            pc::writef(STDERR_FILENO, "[vr] dynres: %.1f fps after the step down (%.1f before), GPU %.0f%%: not the "
                       "GPU; no further step down for 30 s\n", fps, s.fpsBeforeDown, busy);
        }
        s.fpsBeforeDown = 0.0f;
    }
    if (fps < lowFps) {
        s.high = 0;
        s.low++;
    } else if (fps >= highFps && showsMost) {
        s.low = 0;
        s.high++;
    } else {
        s.low = s.high = 0;
    }
    if (s.level > 0 && s.upWait > kUpWindows && now - s.changeNs >= kStableNs) {
        s.upWait = kUpWindows;  // a level held a minute: the wait to go up back to 10 s
    }

    int next = s.level;
    const char* why = nullptr;
    if (s.low >= kDownWindows && s.level < floorLevel && now >= s.holdDownUntil && pc_veg_distance_saturated()) {
        // far under (a GPU-bound scene at 10-15 fps): two steps at once
        next = std::min(floorLevel, s.level + (fps < 0.6f * lowFps ? 2 : 1));
        why = "low";
        if (s.upNs != 0 && now - s.upNs < kUnstableNs) {
            s.upWait = s.upWait * 2 > kUpWindowsMax ? kUpWindowsMax : s.upWait * 2;
        }
        s.fpsBeforeDown = fps;
    } else if (s.high >= s.upWait && s.level > 0) {
        next = s.level - 1;
        why = "steady";
        s.upNs = now;
    }
    if (next != s.level) {
        pc::writef(STDERR_FILENO, "[vr] dynres: %.1f fps (%s; game %.1f, shown %.1f, GPU %.0f%%): %d -> %d lines "
                   "(%.2f of %d)%s\n", fps, why, gameFps, shownFps, busy,
                   linesAt(lines, s.level), linesAt(lines, next), kFactors[next], lines,
                   next < s.level ? "" : s.upWait > kUpWindows ? "; next step up after a longer wait" : "");
        s.level = next;
        s.changeNs = now;
        s.low = s.high = 0;
        s.lastNs = 0;
    }
    return linesAt(lines, s.level);
#else
    (void)base;
    (void)idle;
    return lines;
#endif
}
