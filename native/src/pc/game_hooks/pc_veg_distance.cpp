// The vegetation's adaptive draw distance (xr.ini vegetation_adaptive, vegetation_min, vegetation_max; the
// headset panel's "Vegetación adaptativa"; Android only, every mode, on by default): the palms
// (Obj_Lpalm), the grass, the bushes and small trees (dWood, dTree) and the flowers are all drawn while
// the GPU keeps up; when it does not, the farthest from Link are left out first, step by step, and come
// back the same way once the load falls. Outset among its palms, pigs and villagers kept the GPU at 99%
// at its top clock (640 MHz, 1 frame in 4 lost) with the CPU at 75%: the cost is the GPU's.
//
// - The load, every 500 ms: the GPU busy (wq_xr_gpu_busy: the runtime's metrics, else kgsl's counter)
//   and the frames the headset lost (wq_xr_frames_lost). Overloaded only when the frame rate falls: 2
//   frames lost or more in the window, or the game under kLowFps (28) in it; a busy GPU that still
//   keeps 30 fps cuts nothing. Relaxed: none lost, the game at its rate and the GPU under kBusyLow (82%:
//   room to bring plants back; without a busy reading, none lost alone).
// - Down: each overloaded window, the distance times 0.8 (120 m, 96, 77... down to vegetation_min),
//   then 1 s for it to show before the next step. Up: after kUpWindows relaxed windows in a row, times
//   1.15 up to vegetation_max (the top: the cut is off there). A step down within 10 s of a step up
//   doubles the wait to go up (3 s, 6, 12... 30 s); 60 s without a step down sets it back.
// - The edge: within the last 15% of the distance (3 m at least) a plant shrinks to the ground instead
//   of popping (its model matrix scaled about its foot); past it, it is left out as if the camera's
//   culling had (the game's own flag), so it costs nothing on the GPU and little on the CPU.
// - While cut, also by projected size: a plant of radius r goes from r * kSizeRatio on if that is
//   nearer (grass and flowers before the bushes), and grass and trees lose the game's wider far plane
//   for them (x1.6 and x1.8 of the camera's): what is too small to see goes first.
// - Never in an event (a cutscene, a dialogue) nor in a first-person view (the telescope, the picto
//   box, aiming): everything drawn, as pc_npc_distance.cpp does for the characters.
// - pc_veg_distance_saturated: at vegetation_min and still overloaded (or the adaptive cut off), for
//   the dynamic resolution (pc_vr_dynres.cpp), the second step: it goes down only then.
#include "pc/pc_vr.h"
#include "pc_internal.h"

#include "d/d_com_inf_game.h"

#if defined(__ANDROID__)
#include "wq_xr.h"
#endif

#include <cmath>
#include <unistd.h>

namespace {

constexpr uint64_t kWindowNs = 500000000ull;
constexpr uint64_t kLongFrameNs = 250000000ull;  // a load or a hitch: the window does not count
constexpr float kLowFps = 28.0f;
constexpr float kBusyLow = 82.0f;
constexpr unsigned kLostHigh = 2;
constexpr float kDown = 0.8f, kUp = 1.15f;
constexpr unsigned kSettleWindows = 2;   // after a step down
constexpr unsigned kUpWindows = 6;       // 3 s relaxed
constexpr unsigned kUpWindowsMax = 60;   // 30 s
constexpr unsigned kTopWindows = 60;     // the last step up, to the top: 30 s relaxed
constexpr uint64_t kUnstableNs = 10000000000ull;
constexpr uint64_t kStableNs = 60000000000ull;
constexpr float kSizeRatio = 160.0f;     // a radius of 0.3 m: 48 m (about 3 pixels across at 540 lines)

bool sOn = false;
float sDist = 0.0f;           // metres; at the top (sMax) the cut is off
float sMin = 20.0f, sMax = 120.0f;
bool sActive = false;         // this frame: plants are cut (the distance under the top, no event)
bool sPalmOn = false;         // this frame: the palms' own distance (palm_draw_distance, no event)
float sPalmUnits = 0.0f;
float sOuterUnits = 0.0f, sBandUnits = 0.0f;
float sLinkX = 0.0f, sLinkY = 0.0f, sLinkZ = 0.0f;
bool sOverloaded = false;     // the last window
uint64_t sLastNs = 0, sWinNs = 0;
unsigned sWinFrames = 0;
uint32_t sLostAt = 0;
float sBusyMax = -1.0f;       // the window's highest reading
unsigned sSettle = 0, sRelaxed = 0, sUpWait = kUpWindows;
uint64_t sUpNs = 0, sDownNs = 0;
unsigned sCut = 0, sAsked = 0;  // plants left out / asked, the last frame (the log)

void step(float to, const char* why, float busy, uint32_t lost) {
    pc::writef(STDERR_FILENO, "[veg] %s: %.0f -> %.0f m (GPU %.0f%%, %u frames lost; %u of %u plants out)\n", why,
               sDist, to, busy, lost, sCut, sAsked);
    sDist = to;
}

} // namespace

float pc_veg_draw_scale(float x, float y, float z, float radius) {
    if (!sActive) return 1.0f;
    sAsked++;
    const float dx = x - sLinkX, dy = y - sLinkY, dz = z - sLinkZ;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    float outer = sOuterUnits;
    if (radius * kSizeRatio < outer) outer = radius * kSizeRatio;
    float band = sBandUnits;
    if (band > outer * 0.5f) band = outer * 0.5f;
    if (d >= outer) {
        sCut++;
        return 0.0f;
    }
    if (d <= outer - band) return 1.0f;
    return (outer - d) / band;
}

float pc_veg_far_scale(float gameScale) { return sActive ? 1.0f : gameScale; }

float pc_veg_palm_scale(float x, float y, float z) {
    float s = pc_veg_draw_scale(x, y, z, 650.0f);
    if (!sPalmOn || s <= 0.0f) return s;
    const float dx = x - sLinkX, dy = y - sLinkY, dz = z - sLinkZ;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d >= sPalmUnits) return 0.0f;
    const float band = sPalmUnits * 0.15f > 300.0f ? sPalmUnits * 0.15f : 300.0f;
    if (d > sPalmUnits - band) {
        const float p = (sPalmUnits - d) / band;
        if (p < s) s = p;
    }
    return s;
}

void pc_veg_scale_mtx(float (*mtx)[4], float scale) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) mtx[r][c] *= scale;
}

int pc_veg_distance_saturated(void) {
#if defined(__ANDROID__)
    return !sOn || (sDist <= sMin + 0.01f && sOverloaded);
#else
    return 1;
#endif
}

namespace pc {

void vegDistanceFrame(unsigned int frames) {
    (void)frames;
#if defined(__ANDROID__)
    WqXrTable table;
    wq_xr_table(&table);
    const float busy = wq_xr_gpu_busy();
    const uint32_t lost = wq_xr_frames_lost();
    const fopAc_ac_c* player = dComIfGp_getPlayer(0);
    // an event, a first-person view or no Link: everything drawn, and its load says nothing about the
    // plants (no step, the window starts over)
    const bool allDrawn = player == nullptr || drawDistanceAllDrawn();
    bool restart = false;
    if (table.vegAdaptive != sOn || table.vegMax != sMax || table.vegMin != sMin) {
        if (table.vegAdaptive != sOn) {
            writef(STDERR_FILENO, "[veg] adaptive vegetation %s (%.0f to %.0f m from Link)\n",
                   table.vegAdaptive ? "on" : "off", table.vegMin, table.vegMax);
            restart = true;
        }
        const bool atTop = sDist >= sMax;
        sOn = table.vegAdaptive;
        sMin = table.vegMin;
        sMax = table.vegMax;
        if (!sOn || atTop || sDist <= 0.0f || sDist > sMax) sDist = sMax;
        if (sDist < sMin) sDist = sMin;
    }
    const uint64_t now = monotonicNs();
    const uint64_t dt = sLastNs != 0 ? now - sLastNs : 0;
    sLastNs = now;
    if (sOn && busy > sBusyMax) sBusyMax = busy;
    if (restart || !sOn || allDrawn || dt > kLongFrameNs || sLostAt > lost) {
        // a load, a hitch, a new session, an event, or the cut just turned on: start the window over
        if (restart || !sOn || allDrawn) {
            sOverloaded = false;
            sSettle = sRelaxed = 0;
        }
        sWinNs = 0;
        sWinFrames = 0;
        sLostAt = lost;
        sBusyMax = -1.0f;
    } else if (sOn && dt != 0) {
        sWinNs += dt;
        sWinFrames++;
    }
    if (sOn && sWinNs >= kWindowNs) {
        const uint32_t lostIn = lost - sLostAt;
        const float b = sBusyMax;
        const float fps = (float)(sWinFrames * 1e9 / (double)sWinNs);
        sWinNs = 0;
        sWinFrames = 0;
        sLostAt = lost;
        sBusyMax = -1.0f;
        sOverloaded = lostIn >= kLostHigh || fps < kLowFps;
        const bool relaxed = lostIn == 0 && fps >= kLowFps && (b < 0.0f || b < kBusyLow);
        if (sUpWait > kUpWindows && now - sDownNs >= kStableNs) sUpWait = kUpWindows;
        if (sSettle > 0) {
            sSettle--;
        } else if (sOverloaded && sDist > sMin) {
            if (sUpNs != 0 && now - sUpNs < kUnstableNs)
                sUpWait = sUpWait * 2 > kUpWindowsMax ? kUpWindowsMax : sUpWait * 2;
            const float to = sDist * kDown < sMin ? sMin : sDist * kDown;
            step(to, "load", b, lostIn);
            sSettle = kSettleWindows;
            sDownNs = now;
            sRelaxed = 0;
        } else if (relaxed && sDist < sMax) {
            // the last step to the top turns the cut off (the size limit, the far planes): all comes back
            // at once, so it waits for kTopWindows relaxed (30 s) at least
            const unsigned wait = sDist * kUp >= sMax && sUpWait < kTopWindows ? kTopWindows : sUpWait;
            if (++sRelaxed >= wait) {
                const float to = sDist * kUp > sMax ? sMax : sDist * kUp;
                step(to, "steady", b, lostIn);
                sUpNs = now;
                sRelaxed = 0;
            }
        } else if (!relaxed) {
            sRelaxed = 0;
        }
    }
    sActive = sOn && sDist < sMax && !allDrawn;
    sPalmOn = table.palmDrawDistance > 0.0f && !allDrawn;
    sPalmUnits = table.palmDrawDistance * 100.0f;
    if (sActive || sPalmOn) {
        sLinkX = player->current.pos.x;
        sLinkY = player->current.pos.y;
        sLinkZ = player->current.pos.z;
        sOuterUnits = sDist * 100.0f;
        sBandUnits = sOuterUnits * 0.15f > 300.0f ? sOuterUnits * 0.15f : 300.0f;
    }
#endif
    sCut = sAsked = 0;  // the next frame's draws
}

} // namespace pc
