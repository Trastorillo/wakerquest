// The diorama in the Meta Quest (pc_vr.h): the world as a scaled-down model on a table in the room,
// after Sunshine Quest's gx_vr.cpp (itself after GalaxyQuest's vr_rig.cpp).
//
// - The rig: a pivot follows Link (dead zones along the ground and up and down, so walking and jumps
//   do not drag the table around) and the world's heading follows the game camera's yaw, smoothly,
//   so the stick still means "away from the camera". Stage space is the headset's LOCAL space
//   (metres); the pivot lands at the table's middle, table_distance in front of the eyes and
//   table_height below them, and table_span metres of the game's world cross the table's width.
// - The draws: the painter's world block (pc_vr_view_begin/_end around m_Do_graphic.cpp's camera
//   block) records Aurora's view delta (patch 0019) with C = V_head * Stage * World * V_game^-1, which
//   turns the game camera's view space (baked into every draw's matrices) into the head's, and the
//   projection swapped for one frustum that holds both eyes' (the XR layer's, WqXrHead). Each eye is
//   the head moved by half the distance between the eyes: a constant clip-space shift (Aurora patch
//   0025, aurora_xr_set_eye_shift). The frame carries the pose it was drawn for (the XR layer's tag,
//   aurora_xr_set_frame_info), so the XR layer shows it as a projection layer for that pose and the
//   compositor reprojects the head's moves since.
// - Draws with other projections (the 2D, the HUD, the menus' models) are drawn as before. The sun,
//   the stars and the sky's clouds (built around the camera's eye) are left out.
// - The window (mode = ventana, windowBegin): the same frames, without the rig: the world at its own
//   size behind a rectangle in the room, the game camera's eye a little in front of it (on the player's
//   side) looking through it, each eye looking through it from where it is (the eyes' mask as a portal,
//   Aurora patch 0030), what is between the camera and the window coming out of it.
// Phase 1: no cut at the table's edges and no culling from the head yet; the HUD stays in the picture.
#include "d/dolzel.h" // IWYU pragma: keep
#include "pc/pc_vr.h"

#if defined(__ANDROID__)

#include "pc/pc_harness.h"
#include "pc/pc_settings.h"
#include "pc_internal.h"

#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_view.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "SSystem/SComponent/c_counter.h"
#include "pc/game_hooks.h"
#include "d/d_kankyo.h"
#include "d/d_meter.h"
#include "m_Do/m_Do_lib.h"

#include <algorithm>

#include <aurora/xr.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/mtx.h>

#include "wq_xr.h"

#include <cmath>
#include <cstring>
#include <time.h>
#include <unistd.h>

namespace {

// 4x4 matrices, row-major like GX's Mtx: m[row][col], points are columns (x' = M x).
struct M4 {
    float m[4][4];
};

M4 identity() {
    M4 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
    return r;
}

M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

M4 fromMtx(const Mtx v) {
    M4 r = identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) r.m[i][j] = v[i][j];
    return r;
}

// the inverse of a rigid 3x4 (rotation and translation): R^T, -R^T t
M4 rigidInverse(const Mtx v) {
    M4 r = identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = v[j][i];
    for (int i = 0; i < 3; i++) r.m[i][3] = -(v[0][i] * v[0][3] + v[1][i] * v[1][3] + v[2][i] * v[2][3]);
    return r;
}

float blend(double dt, float halfLife) { return halfLife > 0.0f ? float(1.0 - std::exp2(-dt / halfLife)) : 1.0f; }

// Rig tunables (game units; Wind Waker's are about centimetres, as Sunshine's)
const float kUnitsPerMetre = 100.0f;
const float kSnapDistance = 3000.0f;   // farther: a cut, the world jumps instead of gliding
const float kFollowDeadZone = 100.0f;  // along the ground before the world follows
const float kJumpDeadZone = 350.0f;    // up and down before the world follows (jumps hold still)
const float kPosHalfLife = 0.12f, kRiseHalfLife = 0.25f, kYawHalfLife = 0.35f;
const float kNear = 0.03f, kFar = 200.0f;  // metres
const float kWindowFar = 3000.0f;          // metres at least: the window shows the world at its own size

struct State {
    bool rigValid = false;
    float pivot[3] = {0, 0, 0};
    float yaw = 0.0f;  // the world's heading, as the game camera's yaw
    uint64_t lastNs = 0;
    bool on = false;       // the delta is on in this paint
    bool skyOn = false;
    bool wasOn = false;    // for the log and aurora_xr_set_frame_info
    Mtx delta, skyDelta;
    f32 projFrom[6], projTo[6];
    uint64_t lastLogNs = 0;
    // the table, for the mask and the culling: table coordinates (x, z within +-1 over the top, y 0
    // on it, -1 at the cut below) from the world, and the scale
    bool tableValid = false;
    bool sphere = false;
    M4 tableFromWorld;
    float k = 0.0f;  // metres per game unit
    float halfX = 0.5f, halfZ = 0.35f, below = 0.175f;
    // culling: table from the current camera's view (redone when the view changes)
    Mtx cullView;
    M4 tableFromView;
    bool cullViewValid = false;
    unsigned long cullTests = 0, cullHidden = 0;
    uint64_t cullLogNs = 0;
    // the window (mode = ventana): its table space (x, y within +-1 over its rectangle, z in metres,
    // its plane at 0, +z toward the player) and, in metres, its half sizes, the head in that space, the
    // game camera's eye in front of it and what may come out of it
    bool window = false;
    float winHalfW = 0.8f, winHalfH = 0.45f;
    float winHead[3] = {0, 0, 1};
    float winDepth = 0.5f, winPopout = 1.0f;
    float winDrawUnits = 80000.0f;  // the window's draw distance (game units from the camera)
    unsigned long cullFarHidden = 0;
    // the bubble's focus (the talk zoom, a special scene): the pivot drawn toward `focus` and the span
    // times `focusZoom`, by focusAmt (0 following Link .. 1 on the focus), eased in and out
    float focusAmt = 0.0f;
    float focus[3] = {0, 0, 0};
    bool focusValid = false;
    float focusZoom = 1.0f;
    uint64_t focusNs = 0;
    float view[3] = {0, 0, 0};  // the pivot drawn with: the rig's, or toward the focus
};
State s;

uint64_t nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// GXSetProjection's six perspective parameters of a 4x4 projection
void projParams(const Mtx44 p, f32 out[6]) {
    out[0] = p[0][0];
    out[1] = p[0][2];
    out[2] = p[1][1];
    out[3] = p[1][2];
    out[4] = p[2][2];
    out[5] = p[2][3];
}

// The game camera's yaw from its view matrix: the forward axis is -row 2.
float cameraYaw(const Mtx v) {
    float fx = -v[2][0], fz = -v[2][2];
    if (fx * fx + fz * fz < 1e-6f) {  // looking straight down: the picture's top
        fx = v[1][0];
        fz = v[1][2];
    }
    return std::atan2(fx, -fz);
}

// The world's axes in stage space: right, up, back for stage X, Y, Z.
void axes(float yaw, float r[3], float u[3], float b[3]) {
    const float fx = std::sin(yaw), fz = -std::cos(yaw);  // forward
    u[0] = 0, u[1] = 1, u[2] = 0;
    r[0] = -fz, r[1] = 0, r[2] = fx;  // forward x up
    b[0] = -fx, b[1] = 0, b[2] = -fz;
}

void updateRig(const cXyz& link, const Mtx view, bool followYaw) {
    const uint64_t now = nowNs();
    double dt = s.lastNs ? double(now - s.lastNs) * 1e-9 : 0.0;
    s.lastNs = now;
    const float m[3] = {link.x, link.y, link.z};
    const float camYaw = cameraYaw(view);
    const float d[3] = {m[0] - s.pivot[0], m[1] - s.pivot[1], m[2] - s.pivot[2]};
    const bool cut = !s.rigValid || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > kSnapDistance * kSnapDistance;
    if (cut) {
        memcpy(s.pivot, m, sizeof m);
        s.yaw = camYaw;
        s.rigValid = true;
        return;
    }
    if (dt > 0.25) dt = 0.25;
    const float n = std::sqrt(d[0] * d[0] + d[2] * d[2]);
    if (n > kFollowDeadZone) {
        const float k = blend(dt, kPosHalfLife) * (1.0f - kFollowDeadZone / n);
        s.pivot[0] += d[0] * k;
        s.pivot[2] += d[2] * k;
    }
    const float h = std::fabs(d[1]);
    if (h > kJumpDeadZone) s.pivot[1] += d[1] * (1.0f - kJumpDeadZone / h) * blend(dt, kPosHalfLife);
    else s.pivot[1] += d[1] * blend(dt, kRiseHalfLife) * (h / kJumpDeadZone);
    if (followYaw) {
        float dy = camYaw - s.yaw;
        while (dy > 3.14159265f) dy -= 6.28318531f;
        while (dy < -3.14159265f) dy += 6.28318531f;
        s.yaw += dy * blend(dt, kYawHalfLife);
    }
}

// The bubble's focus (the talk zoom, a special scene), every frame: `target` (or none) and the span's
// zoom while on it. The pivot glides toward it and the span shrinks over about a second (smoothstep
// of a linear ramp), the target followed with a short half-life; then back to the rig's pivot (Link).
// The rig keeps following Link meanwhile, so the way back lands on him.
const float kFocusSeconds = 1.0f, kFocusHalfLife = 0.2f;

void updateFocus(const float* target, float zoom) {
    const uint64_t now = nowNs();
    double dt = s.focusNs ? double(now - s.focusNs) * 1e-9 : 0.0;
    s.focusNs = now;
    if (dt > 0.25) dt = 0.25;
    if (target != nullptr) {
        if (!s.focusValid || s.focusAmt <= 0.0f) {
            memcpy(s.focus, target, sizeof s.focus);
        } else {
            const float k = blend(dt, kFocusHalfLife);
            for (int i = 0; i < 3; i++) s.focus[i] += (target[i] - s.focus[i]) * k;
        }
        s.focusValid = true;
        s.focusZoom = zoom;
    }
    const float want = target != nullptr ? 1.0f : 0.0f;
    const float step = float(dt) / kFocusSeconds;
    s.focusAmt = want > s.focusAmt ? std::min(want, s.focusAmt + step) : std::max(want, s.focusAmt - step);
    if (s.focusAmt <= 0.0f) s.focusValid = false;
}

// The eased focus amount (0 .. 1)
float focusEase() {
    const float a = s.focusValid ? s.focusAmt : 0.0f;
    return a * a * (3.0f - 2.0f * a);
}

// Stage from world: the world's axes scaled to metres, the pivot (s.view: the rig's, or toward the
// focus) at the table's middle.
M4 stageFromWorld(const WqXrTable& t) {
    float r[3], u[3], b[3];
    axes(s.yaw + t.yaw, r, u, b);  // the grip's turn on top of the camera's heading
    const float k = t.width / (t.span * kUnitsPerMetre);  // metres per game unit
    M4 m = identity();
    for (int c = 0; c < 3; c++) {
        m.m[0][c] = r[c] * k;
        m.m[1][c] = u[c] * k;
        m.m[2][c] = b[c] * k;
    }
    const float anchor[3] = {t.x, -t.height, -t.distance};
    for (int row = 0; row < 3; row++)
        m.m[row][3] = anchor[row] - (m.m[row][0] * s.view[0] + m.m[row][1] * s.view[1] + m.m[row][2] * s.view[2]);
    return m;
}

// Head from stage: the inverse of the head's pose (quaternion and position in LOCAL space).
M4 headFromStage(const WqXrHead& h) {
    const float x = h.rot[0], y = h.rot[1], z = h.rot[2], w = h.rot[3];
    const float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    M4 m = identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m.m[i][j] = R[j][i];  // R^T
    for (int i = 0; i < 3; i++)
        m.m[i][3] = -(R[0][i] * h.pos[0] + R[1][i] * h.pos[1] + R[2][i] * h.pos[2]);
    return m;
}

void toMtx(const M4& a, Mtx out) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) out[i][j] = a.m[i][j];
}

// Table from stage: its middle at the anchor (table_distance ahead, table_height below the eyes).
M4 tableFromStage(const WqXrTable& t) {
    M4 m = identity();
    if (t.sphere) {
        // the bubble: radius 1 around its middle, 0.35 radii above Link
        const float r = 0.5f * t.width;
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f / r;
        m.m[0][3] = -t.x / r;
        m.m[1][3] = (t.height - 0.35f * r) / r;
        m.m[2][3] = t.distance / r;
        return m;
    }
    m.m[0][0] = 1.0f / s.halfX;
    m.m[0][3] = -t.x / s.halfX;
    m.m[1][1] = 1.0f / s.below;
    m.m[1][3] = t.height / s.below;
    m.m[2][2] = 1.0f / s.halfZ;
    m.m[2][3] = t.distance / s.halfZ;
    return m;
}

// Stage from head: the head's pose (the inverse of headFromStage)
M4 stageFromHead(const WqXrHead& h) {
    const float x = h.rot[0], y = h.rot[1], z = h.rot[2], w = h.rot[3];
    M4 m = identity();
    const float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m.m[i][j] = R[i][j];
        m.m[i][3] = h.pos[i];
    }
    return m;
}

void xform(const M4& a, const float p[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = a.m[r][0] * p[0] + a.m[r][1] * p[1] + a.m[r][2] * p[2] + a.m[r][3];
}

// The window (mode = ventana): a rectangle in the room (its pose: windowX, windowY, -windowDistance,
// turned windowYaw about the vertical) the world is seen through at its own size (windowScale of the
// game's metres per metre). The game camera's eye sits windowDepth in front of the window's middle (on
// the player's side), looking through it (the camera's view space looks down -z, as the window's own
// space away from the player): Room from the game camera's view = WindowPose * T(0, 0, depth) * S(k), k
// metres per game unit. What the camera sees nearer than depth lies in front of the window (it comes
// out); what is behind the camera, past z = depth, is never shown. With the stage being the room
// (LOCAL), C = V_head * that: no rig, the camera's moves are the world's.
M4 roomFromWindow(const WqXrTable& t) {
    const float c = std::cos(t.windowYaw), sn = std::sin(t.windowYaw);
    M4 m = identity();
    m.m[0][0] = c, m.m[0][2] = sn;
    m.m[2][0] = -sn, m.m[2][2] = c;
    m.m[0][3] = t.windowX;
    m.m[1][3] = t.windowY;
    m.m[2][3] = -t.windowDistance;
    return m;
}

M4 roomFromCameraView(const WqXrTable& t, float k) {
    M4 back = identity();
    back.m[2][3] = t.windowDepth;
    M4 scale = identity();
    scale.m[0][0] = scale.m[1][1] = scale.m[2][2] = k;
    return mul(mul(roomFromWindow(t), back), scale);
}

// The window's table space from the room: the window's pose undone, x and y over its half sizes.
M4 windowTableFromRoom(const WqXrTable& t) {
    const M4 pose = roomFromWindow(t);
    M4 inv = identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) inv.m[i][j] = pose.m[j][i];
    for (int i = 0; i < 3; i++)
        inv.m[i][3] = -(pose.m[0][i] * pose.m[0][3] + pose.m[1][i] * pose.m[1][3] + pose.m[2][i] * pose.m[2][3]);
    M4 scale = identity();
    scale.m[0][0] = 1.0f / (0.5f * t.windowWidth);
    scale.m[1][1] = 1.0f / (0.5f * t.windowHeight);
    return mul(scale, inv);
}

// A sphere (the window's table space, radius in metres) against what the window shows, conservatively
// (a margin for the head moving before the frame is shown): behind the plane, the four planes through
// the head and the window's edges; in front, the pop-out's volume (the camera's near frustum: the
// rectangle narrowing toward the game camera's eye, depth in front of it). Nothing beyond the pop-out,
// nor behind the camera, comes out. 1 culled, 0 drawn.
const float kWindowCameraGap = 0.02f;  // m: the pop-out stops this short of the game camera's eye

int cullWindow(const float t[3], float r) {
    const float margin = 0.25f;
    const float p[3] = {t[0] * s.winHalfW, t[1] * s.winHalfH, t[2]};  // metres
    const float* e = s.winHead;
    bool seen = false;
    if (p[2] - r < 0.0f) {
        if (e[2] < 0.05f) {
            seen = true;  // the head at (or through) the window's plane: no test
        } else {
            seen = true;
            const float half[2] = {s.winHalfW + margin, s.winHalfH + margin};
            for (int axis = 0; axis < 2 && seen; axis++)
                for (int side = -1; side <= 1 && seen; side += 2) {
                    // the plane through the head and the edge at side * half (along the other axis):
                    // outward normal (side * e.z, side * (side * half - e)) in (axis, z)
                    const float edge = side * half[axis];
                    const float nx = side * e[2], nz = side * (edge - e[axis]);
                    const float len = std::sqrt(nx * nx + nz * nz);
                    if (len < 1e-6f) continue;
                    const float dist = (nx * (p[axis] - edge) + nz * p[2]) / len;
                    if (dist > r) seen = false;
                }
        }
    }
    const float popout = std::min(s.winPopout, s.winDepth - kWindowCameraGap);
    if (!seen && popout > 0.0f && p[2] + r >= 0.0f && p[2] - r <= popout) {
        // the rectangle's scale at the sphere's nearest height to the window (the widest it is there)
        const float narrow = (s.winDepth - std::max(p[2] - r, 0.0f)) / s.winDepth;
        seen = std::fabs(p[0]) - r <= s.winHalfW * narrow + margin && std::fabs(p[1]) - r <= s.winHalfH * narrow + margin;
    }
    return seen ? 0 : 1;
}

// The culling's table from the current camera view (dComIfGd_getView, set by camera_draw before the
// actors draw), false when the diorama is off.
bool cullSetup() {
    if (!s.tableValid) return false;
    view_class* view = dComIfGd_getView();
    if (view == nullptr) return false;
    if (!s.cullViewValid || memcmp(s.cullView, view->mViewMtx, sizeof(Mtx)) != 0) {
        memcpy(s.cullView, view->mViewMtx, sizeof(Mtx));
        s.tableFromView = mul(s.tableFromWorld, rigidInverse(view->mViewMtx));
        s.cullViewValid = true;
    }
    return true;
}

// The birds flying around the room (birds_free): the seagulls, the small birds, the Kargarocs, the
// Peahats, the Keese... within bird_range table radii, each a sphere in the table's space (radius from
// its size) that the eyes' mask keeps outside the table (Aurora patch 0029) and the culling draws.
struct Birds {
    float t[AURORA_XR_FREE_SPHERES][4];
    int n = 0;
    float range = 3.0f;
};
Birds sBirds;

int collectBird(void* actor, void*) {
    fopAc_ac_c* ac = (fopAc_ac_c*)actor;
    float size;  // game units around its middle
    switch (fopAcM_GetName(ac)) {
    case fpcNm_KAMOME_e:
    case fpcNm_DEMO_KMM_e:
    case fpcNm_KT_e:
    case fpcNm_WBIRD_e:
    case fpcNm_KI_e: size = 150.0f; break;
    case fpcNm_PH_e: size = 250.0f; break;
    case fpcNm_BB_e:
    case fpcNm_DK_e: size = 450.0f; break;
    default: return 1;
    }
    if (sBirds.n >= AURORA_XR_FREE_SPHERES) return 1;
    const float p[3] = {ac->current.pos.x, ac->current.pos.y, ac->current.pos.z};
    float t[3];
    xform(s.tableFromWorld, p, t);
    if (std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]) > sBirds.range) return 1;
    float* b = sBirds.t[sBirds.n++];
    b[0] = t[0], b[1] = t[1], b[2] = t[2];
    b[3] = size * s.k / s.halfX;
    return 1;
}

// The room's ambience (ambient, mode = pantalla; wakerquest-files/ambiente-plan.md): the game's own
// ambient actors (the seagulls, the crabs, the small birds, the fireflies), its wind lines and seagull
// emitters and its rain, drawn a second time outside the giant screen, in the room. The screen is seen
// as a window from a point in front of it where its picture matches the game camera's (the frustum's
// height across the screen's), the camera's view space scaled so that what is ambient_focus game units
// away lies on the screen's plane: what is out of the camera's frame is in the room around the screen,
// where it would be. Aurora (patch 0034) draws those draws for each eye and hides them where the
// screen is, fading them out away from it. Step 2: one seagull (the nearest to Link) as a test.
struct Ambient {
    unsigned n[5] = {};  // KAMOME (and KAMOME2, the same process here), DEMO_KMM, KN, KT, FF
    uint64_t lastLogNs = 0;
    bool on = false;      // this frame's painter draws the ambience (pc_vr_ambient_draw)
    bool wasOn = false;
    Mtx delta;            // the game camera's view space to the head's
    f32 projFrom[6], projTo[6];
    // the packets taken from the ambient actor's draw (J3DJoint::entryIn), for one game frame
    struct Rec {
        J3DMatPacket* mat;
        J3DShapePacket* shape;
    };
    Rec rec[256];  // Link's models hold many
    int recN = 0;
    u32 recFrame = 0;
    const fopAc_ac_c* best = nullptr;  // the test's seagull, nearest to Link
    float bestDist = 0.0f;
    cXyz link;
    // the log's: the painter's calls since it last wrote, the ones that drew, and why the others did not
    unsigned paints = 0, drawn = 0, stale = 0, noView = 0;
};
Ambient sAmbient;

// 0..4 for an ambient actor's kind, -1 for any other
int ambientKind(s16 name) {
    switch (name) {
    case fpcNm_KAMOME_e: return 0;
    case fpcNm_DEMO_KMM_e: return 1;
    case fpcNm_KN_e: return 2;
    case fpcNm_KT_e: return 3;
    case fpcNm_FF_e: return 4;
    default: return -1;
    }
}

int collectAmbient(void* actor, void*) {
    const fopAc_ac_c* ac = (const fopAc_ac_c*)actor;
    const int kind = ambientKind(fopAcM_GetName((fopAc_ac_c*)ac));
    if (kind < 0) return 1;
    sAmbient.n[kind]++;
    if (kind == 0) {
        const float d = (ac->current.pos - sAmbient.link).abs();
        if (sAmbient.best == nullptr || d < sAmbient.bestDist) {
            sAmbient.best = ac;
            sAmbient.bestDist = d;
        }
    }
    return 1;
}

void ambientOff() {
    if (sAmbient.wasOn) {
        aurora_xr_set_ambient(false, 0, nullptr, nullptr, 0.0f, nullptr, 0.0f, 0.0f, 0.0f);
        pc::writef(STDERR_FILENO, "[vr] ambient off\n");
        sAmbient.wasOn = false;
    }
    sAmbient.on = false;
    pc_vr_ambient_actor = nullptr;
}

// The giant screen's frame (pc_vr_view_begin, mode = pantalla): which actor is ambient, and the
// ambience's view delta, frustum and mask for this frame's draws.
void ambientFrame(view_class* view, const WqXrTable& table, const fopAc_ac_c* player) {
    WqXrHead head;
    if (!table.ambient || view == nullptr || player == nullptr || !wq_xr_head(&head) || head.tag == 0 ||
        view->mProjMtx[3][3] != 0.0f || view->mProjMtx[0][0] == 0.0f) {
        ambientOff();
        return;
    }
    sAmbient.link = player->current.pos;
    sAmbient.best = nullptr;
    memset(sAmbient.n, 0, sizeof(sAmbient.n));
    fopAcIt_Executor(collectAmbient, nullptr);
    pc_vr_ambient_actor = table.ambientTestLink ? (const void*)player : (const void*)sAmbient.best;

    // the screen as a window (roomFromWindow's fields), its picture's aspect the game projection's
    const float p00 = view->mProjMtx[0][0], p11 = view->mProjMtx[1][1];
    WqXrTable scr = table;
    scr.windowX = table.screenX;
    scr.windowY = table.screenY;
    scr.windowDistance = table.screenDistance;
    scr.windowYaw = table.screenYaw;
    scr.windowWidth = table.screenWidth;
    scr.windowHeight = table.screenWidth * p00 / p11;
    // the point in front of the screen whose view of it is the camera's frustum, and the scale that
    // puts ambient_focus game units on its plane
    scr.windowDepth = 0.5f * scr.windowHeight * p11;
    const float k = scr.windowDepth / std::max(table.ambientFocus, 1.0f);  // metres per game unit
    const M4 c = mul(headFromStage(head), roomFromCameraView(scr, k));
    toMtx(c, sAmbient.delta);
    projParams(view->mProjMtx, sAmbient.projFrom);
    const float gameFar =
        std::fabs(sAmbient.projFrom[4]) > 1e-9f ? std::fabs(sAmbient.projFrom[5] / sAmbient.projFrom[4]) : 0.0f;
    const float far = std::max(kWindowFar, gameFar * k);
    const float fov[4] = {head.tanLeft, head.tanRight, head.tanDown, head.tanUp};
    Mtx44 p;
    C_MTXFrustum(p, kNear * fov[3], kNear * fov[2], kNear * fov[0], kNear * fov[1], kNear, far);
    projParams(p, sAmbient.projTo);
    const float eyeShift = -p[0][0] * head.ipd * 0.5f;
    const M4 screenHead = mul(windowTableFromRoom(scr), stageFromHead(head));
    float sfh[12];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) sfh[i * 4 + j] = screenHead.m[i][j];
    const float half[2] = {0.5f * scr.windowWidth, 0.5f * scr.windowHeight};
    wq_xr_set_frame_view(head.tag, false, nullptr);
    // the edge's softness; negative: not hidden where the screen is (ambient_mask 0)
    aurora_xr_set_ambient(true, head.tag, sfh, sAmbient.projTo, eyeShift, half, table.ambientRange,
                          0.4f * table.ambientRange, table.ambientMask ? 0.03f : -0.03f);
    sAmbient.on = true;

    const uint64_t now = nowNs();
    if (!sAmbient.wasOn || now - sAmbient.lastLogNs > 5000000000ull) {
        sAmbient.lastLogNs = now;
        pc::writef(STDERR_FILENO,
                   "[vr] ambient: seagulls %u (demo %u), crabs %u, small birds %u, fireflies %u; rain %d; test "
                   "%s, %d packets; screen %.2f x %.2f m, eye point %.2f m in front, "
                   "%.4f m per unit, range %.1f m; paints %u (drawn %u, stale %u, no view %u)\n",
                   sAmbient.n[0], sAmbient.n[1], sAmbient.n[2], sAmbient.n[3], sAmbient.n[4], g_env_light.mRainCount,
                   table.ambientTestLink ? "Link" : sAmbient.best != nullptr ? "the nearest seagull" : "no seagull",
                   sAmbient.recN,
                   scr.windowWidth, scr.windowHeight, scr.windowDepth, k, table.ambientRange, sAmbient.paints,
                   sAmbient.drawn, sAmbient.stale, sAmbient.noView);
        sAmbient.paints = sAmbient.drawn = sAmbient.stale = sAmbient.noView = 0;
    }
    sAmbient.wasOn = true;
}

// The ambient actor's clips: never culled, so what flies out of the camera's frame is still drawn
bool ambientClipActor() {
    return sAmbient.on && pc_vr_ambient_actor != nullptr && pc_vr_clip_actor == pc_vr_ambient_actor;
}

// The census's window line (COS_CENSUS, pc_census.cpp; logging only): what passes the window's
// culling, by who asked (pc_vr_clip_actor's process name, else pc_vr_clip_kind, else "untagged"), per
// second with the culling's own line; the tests let through by kKeepRadius (beyond the window's draw
// distance but large) and the BG's (the rooms' terrain, d_a_bg / d_a_lod_bg) apart.
struct WinCensus {
    static constexpr int kKinds = 12;
    static constexpr int kSlots = fpcNm_MAX_NUM_e + kKinds + 1;  // the actors, the kinds, untagged
    uint32_t tests[kSlots];
    uint32_t drawn[kSlots];
    const char* kind[kKinds];
    int kinds = 0;
    uint32_t keep = 0, keepDrawn = 0;
};
WinCensus sWin;

int winCensusSlot() {
    if (pc_vr_clip_actor != nullptr) {
        const int name = fopAcM_GetName((void*)pc_vr_clip_actor);
        if (name >= 0 && name < fpcNm_MAX_NUM_e) return name;
    } else if (pc_vr_clip_kind != nullptr) {
        for (int i = 0; i < sWin.kinds; i++)
            if (sWin.kind[i] == pc_vr_clip_kind || strcmp(sWin.kind[i], pc_vr_clip_kind) == 0) return fpcNm_MAX_NUM_e + i;
        if (sWin.kinds < WinCensus::kKinds) {
            sWin.kind[sWin.kinds] = pc_vr_clip_kind;
            return fpcNm_MAX_NUM_e + sWin.kinds++;
        }
    }
    return WinCensus::kSlots - 1;
}

const char* winCensusName(int slot) {
    if (slot < fpcNm_MAX_NUM_e) return pc::censusProcName(slot);
    if (slot < WinCensus::kSlots - 1) return sWin.kind[slot - fpcNm_MAX_NUM_e];
    return "untagged";
}

void winCensusLog() {
    char line[1024];
    size_t len = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (len >= sizeof(line) - 1) return;
        const int n = snprintf(line + len, sizeof(line) - len, fmt, a...);
        if (n > 0) len = std::min(len + (size_t)n, sizeof(line) - 1);
    };
    uint32_t tests = 0, drawn = 0;
    for (int i = 0; i < WinCensus::kSlots; i++) tests += sWin.tests[i], drawn += sWin.drawn[i];
    put("[census] window: %u tests, %u drawn; kKeepRadius kept %u (%u drawn); BG %u/%u, LOD_BG %u/%u; drawn by kind",
        tests, drawn, sWin.keep, sWin.keepDrawn, sWin.drawn[fpcNm_BG_e], sWin.tests[fpcNm_BG_e],
        sWin.drawn[fpcNm_LODBG_e], sWin.tests[fpcNm_LODBG_e]);
    // the top 15 by drawn (each as drawn/tests), picked by clearing what is printed
    for (int printed = 0; printed < 15; printed++) {
        int best = -1;
        for (int i = 0; i < WinCensus::kSlots; i++)
            if (sWin.drawn[i] != 0 && (best < 0 || sWin.drawn[i] > sWin.drawn[best])) best = i;
        if (best < 0) break;
        put("%s %sx%u/%u", printed == 0 ? "" : ",", winCensusName(best), sWin.drawn[best], sWin.tests[best]);
        sWin.drawn[best] = 0;
    }
    pc::writef(STDERR_FILENO, "%s\n", line);
    memset(sWin.tests, 0, sizeof(sWin.tests));
    memset(sWin.drawn, 0, sizeof(sWin.drawn));
    sWin.keep = sWin.keepDrawn = 0;
}

// A sphere (view space, game units) against the table's box (or what the window shows): 1 culled, 0
// drawn. The margin keeps
// what the rig is about to bring onto the table; nothing above the table is cut, nor a bird about the
// room.
int cullSphereView(const float p[3], float radius) {
    float t[3];
    xform(s.tableFromView, p, t);
    const float r = radius * s.k;
    if (s.window) {
        // the window: what it shows, from the head (and coming out of it), as far as the game itself
        // draws it (its clipper's far, which actors' culling sets to their own draw distances and the
        // camera's frustum test would have applied) and the window's draw distance (the horizon:
        // window_draw_distance), which keeps the large models (the sea, the islands' low detail)
        s.cullTests++;
        const float depth = -p[2] - radius;  // game units in front of the game camera
        const float kKeepRadius = 5000.0f;   // 50 m: the sea's and the islands' models
        const bool far = depth > mDoLib_clipper::mClipper.getFar() || (depth > s.winDrawUnits && radius < kKeepRadius);
        const int out = far ? 1 : cullWindow(t, r);
        if (out) s.cullHidden++;
        if (far) s.cullFarHidden++;
        const bool census = pc::censusOn();
        if (census) {
            const int slot = winCensusSlot();
            sWin.tests[slot]++;
            if (!out) sWin.drawn[slot]++;
            if (!far && depth > s.winDrawUnits) {
                sWin.keep++;
                if (!out) sWin.keepDrawn++;
            }
        }
        const uint64_t now = nowNs();
        if (now - s.cullLogNs > 1000000000ull) {
            if (census && s.cullLogNs != 0) winCensusLog();
            if (s.cullLogNs != 0)
                pc::writef(STDERR_FILENO,
                           "[vr] culling by the window: %lu tests, %lu culled in the last second (%lu by distance, "
                           "beyond %.0f m)\n",
                           s.cullTests, s.cullHidden, s.cullFarHidden, s.winDrawUnits / kUnitsPerMetre);
            s.cullTests = s.cullHidden = s.cullFarHidden = 0;
            s.cullLogNs = now;
        }
        return out;
    }
    for (int i = 0; i < sBirds.n; i++) {
        const float* b = sBirds.t[i];
        const float dx = t[0] - b[0], dy = t[1] - b[1], dz = t[2] - b[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) < b[3] + r / s.halfX) return 0;
    }
    const float margin = 0.15f;
    s.cullTests++;
    const bool out = s.sphere ? std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]) - r / s.halfX > 1.0f + margin
                              : std::fabs(t[0]) - r / s.halfX > 1.0f + margin ||
                                    std::fabs(t[2]) - r / s.halfZ > 1.0f + margin || t[1] + r / s.below < -1.0f - margin;
    if (out) s.cullHidden++;
    const uint64_t now = nowNs();
    if (now - s.cullLogNs > 1000000000ull) {
        if (s.cullLogNs != 0)
            pc::writef(STDERR_FILENO, "[vr] culling by the table: %lu tests, %lu culled in the last second\n",
                       s.cullTests, s.cullHidden);
        s.cullTests = s.cullHidden = 0;
        s.cullLogNs = now;
    }
    return out ? 1 : 0;
}

// The window's frustum fitted to what it can show (its rectangle, and the pop-out's front: the rectangle
// narrowed toward the camera's eye at the pop-out's depth), seen from both eyes, as tangents from the
// head's axes (left and down < 0) with a margin for the head moving before the frame is shown, within
// the head's own (which has its margin already). False (the head's frustum) when a corner is at or
// behind the eyes' plane, or the window is out of view. The same pixels then cover only the window:
// more of them per degree.
bool fitWindowFrustum(const WqXrTable& table, const WqXrHead& head, float popout, float out[4]) {
    const M4 headWindow = mul(headFromStage(head), roomFromWindow(table));
    const float hw = 0.5f * table.windowWidth, hh = 0.5f * table.windowHeight;
    float corners[8][3];
    int n = 0;
    for (int sy = -1; sy <= 1; sy += 2)
        for (int sx = -1; sx <= 1; sx += 2) {
            const float c[3] = {sx * hw, sy * hh, 0.0f};
            xform(headWindow, c, corners[n++]);
        }
    if (popout > 0.0f) {
        const float narrow = (table.windowDepth - popout) / table.windowDepth;
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sx = -1; sx <= 1; sx += 2) {
                const float c[3] = {sx * hw * narrow, sy * hh * narrow, popout};
                xform(headWindow, c, corners[n++]);
            }
    }
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (int i = 0; i < n; i++) {
        const float z = -corners[i][2];
        if (z < 0.05f) return false;
        for (int eye = -1; eye <= 1; eye += 2) {
            const float tx = (corners[i][0] - eye * 0.5f * head.ipd) / z, ty = corners[i][1] / z;
            lo[0] = std::min(lo[0], tx), hi[0] = std::max(hi[0], tx);
            lo[1] = std::min(lo[1], ty), hi[1] = std::max(hi[1], ty);
        }
    }
    // the margin: 8% of the span on each side and 0.03 (about 2 degrees)
    for (int a = 0; a < 2; a++) {
        const float m = 0.08f * (hi[a] - lo[a]) + 0.03f;
        lo[a] -= m, hi[a] += m;
    }
    // out is left as it was (the head's) when the fit fails: the window out of the head's frustum
    const float f[4] = {std::max(lo[0], head.tanLeft), std::min(hi[0], head.tanRight), std::max(lo[1], head.tanDown),
                        std::min(hi[1], head.tanUp)};
    if (f[1] - f[0] <= 0.02f || f[3] - f[2] <= 0.02f) return false;
    memcpy(out, f, sizeof f);
    return true;
}

// See Link through what stands in front of him (seethrough_radius, Aurora patch 0031): with the view
// delta, Link's middle in the head's space (headFromWorld: the head from the game's world) and a cone
// from the eyes' midpoint toward him, radius game metres wide at him. The world's fragments within it
// and nearer than him by more than a margin (his own size: his draws, his shield, the ground at his
// feet stay) are discarded, dithered over a soft edge: the trees, the grass and the walls between the
// eyes and Link are cut away, and Link and what is behind him show. The same volume for both eyes.
const float kLinkMiddle = 60.0f;      // game units above his feet
const float kSeeMargin = 100.0f;      // game units before him: what is nearer is cut
const float kSeeSoftDepth = 40.0f;    // game units: the cut fades in over this toward him
const float kSeeSoftEdge = 0.35f;     // of the radius, dithered at the cone's edge

void seeThrough(const M4& headFromWorld, const fopAc_ac_c* player, float radiusMetres, float k) {
    if (radiusMetres <= 0.0f || player == nullptr) return;  // off: Aurora turns it off every frame
    const float p[3] = {player->current.pos.x, player->current.pos.y + kLinkMiddle, player->current.pos.z};
    float l[3];
    xform(headFromWorld, p, l);
    const float dist = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (dist < 1e-3f) return;
    const float r = radiusMetres * kUnitsPerMetre * k;  // metres in the head's space
    const f32 params[8] = {l[0], l[1], l[2], kSeeSoftDepth * k, r / dist, kSeeMargin * k, kSeeSoftEdge, 0.0f};
    AuroraSetSeeThrough(params);
}

// The window's frame (mode = ventana), from pc_vr_view_begin: the view delta from the game camera's view
// to the head's through the window, the head's frustum (both eyes, as the diorama's: the frustum is the
// head's, wider than the window, and the eyes' mask cuts it to what each eye sees through the window),
// the eyes' mask as the window (Aurora patch 0030) and the culling against what it shows.
bool windowBegin(view_class* view, const WqXrTable& table, const WqXrHead& head, const fopAc_ac_c* player) {
    s.window = true;
    s.k = 1.0f / (table.windowScale * kUnitsPerMetre);  // metres per game unit
    s.winHalfW = 0.5f * table.windowWidth;
    s.winHalfH = 0.5f * table.windowHeight;
    s.winDepth = table.windowDepth;
    s.winPopout = table.windowPopout;
    // the draw distance: the same metres of the room whatever the scale (the game's metres times it)
    s.winDrawUnits = table.windowDrawDistance * table.windowScale * kUnitsPerMetre;
    const M4 roomView = roomFromCameraView(table, s.k);
    const M4 tableRoom = windowTableFromRoom(table);
    // the culling's table from the world (cullSetup takes the current camera's view off again)
    s.tableFromWorld = mul(mul(tableRoom, roomView), fromMtx(view->mViewMtx));
    s.tableValid = true;
    s.cullViewValid = false;
    {
        float t[3];
        xform(tableRoom, head.pos, t);
        s.winHead[0] = t[0] * s.winHalfW;
        s.winHead[1] = t[1] * s.winHalfH;
        s.winHead[2] = t[2];
    }

    // C = V_head * WindowPose * T(0, 0, +depth) * S(k)
    const M4 c = mul(headFromStage(head), roomView);
    toMtx(c, s.delta);
    // the far plane: the game's own (its projection: P23 / P22 = far) at the window's scale, at least
    // kWindowFar. Not brought in to the draw distance: with the reversed float depth it costs nothing
    // where it is, and nearer it would cut the sky's dome and the sea's far edge with a hard line; the
    // game's fog already hazes the horizon
    projParams(view->mProjMtx, s.projFrom);
    const float gameFar = std::fabs(s.projFrom[4]) > 1e-9f ? std::fabs(s.projFrom[5] / s.projFrom[4]) : 0.0f;
    const float far = std::max(kWindowFar, gameFar * s.k);
    // the frustum fitted to the window (the XR layer submits the eyes with the same tangents, given
    // with the frame's tag: wq_xr_set_frame_view), else the head's
    const float popout = std::max(0.0f, std::min(table.windowPopout, table.windowDepth - kWindowCameraGap));
    float fov[4] = {head.tanLeft, head.tanRight, head.tanDown, head.tanUp};
    const bool fitted = fitWindowFrustum(table, head, popout, fov);
    wq_xr_set_frame_view(head.tag, true, fitted ? fov : nullptr);
    Mtx44 p;
    C_MTXFrustum(p, kNear * fov[3], kNear * fov[2], kNear * fov[0], kNear * fov[1], kNear, far);
    projParams(p, s.projTo);
    const float eyeShift = -p[0][0] * head.ipd * 0.5f;
    aurora_xr_set_eye_shift(true, 0.0f, eyeShift);
    {
        const M4 tableHead = mul(tableRoom, stageFromHead(head));
        float tfh[12];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) tfh[i * 4 + j] = tableHead.m[i][j];
        // what comes out is opaque up to a thin edge (window_fade) at the pop-out's front; with the
        // pop-out off (window_popout_on 0) nothing in front of the window's plane is shown
        float fade[4] = {table.windowFade / s.winHalfW, table.windowFade / s.winHalfH, std::max(table.windowFade, 0.005f),
                         2.0f};
        aurora_xr_set_diorama_mask(tfh, s.projTo, eyeShift, fade);
        aurora_xr_set_diorama_window(popout, table.windowDepth);
    }
    sBirds.n = 0;  // the birds fly in the world behind the window, at its size
    aurora_xr_set_diorama_free(nullptr, 0);
    aurora_xr_set_frame_info(head.tag, true);
    AuroraSetViewDelta(&s.delta[0][0], s.projFrom, s.projTo);
    seeThrough(mul(c, fromMtx(view->mViewMtx)), player, table.seeThroughRadius, s.k);
    {
        // what the game camera sees in front of the pop-out's front (the window's plane with the pop-out
        // off) is not drawn at all (Aurora patch 0031's clip plane), so what is behind it shows, rather
        // than the mask fading it into a see-through hole: the window's +z (toward the player) in the
        // head's space, through the point popout in front of its middle
        const M4 headWindow = mul(headFromStage(head), roomFromWindow(table));
        const float n[3] = {headWindow.m[0][2], headWindow.m[1][2], headWindow.m[2][2]};
        const float o[3] = {0.0f, 0.0f, popout};
        float at[3];
        xform(headWindow, o, at);
        const f32 plane[4] = {n[0], n[1], n[2], -(n[0] * at[0] + n[1] * at[1] + n[2] * at[2])};
        AuroraSetClipPlane(plane);
    }
    // the sky's things built around the camera's eye: drawn as they are, beyond the window
    memcpy(s.skyDelta, s.delta, sizeof(s.skyDelta));
    s.on = true;
    s.skyOn = false;

    const uint64_t now = nowNs();
    if (!s.wasOn || now - s.lastLogNs > 5000000000ull) {
        pc::writef(STDERR_FILENO,
                   "[vr] window: %.2f x %.2f m at (%.2f, %.2f, %.2f), %.0f deg; %.4f m per unit (scale %.2f), camera "
                   "%.2f m in front, coming out %.2f m, far %.0f m; frustum %s (%.2f %.2f %.2f %.2f); head (%.2f, %.2f, %.2f), %.2f m from the window, tag %u, "
                   "ipd %.3f\n",
                   table.windowWidth, table.windowHeight, table.windowX, table.windowY, -table.windowDistance,
                   table.windowYaw * 57.2958f, s.k, table.windowScale, table.windowDepth, table.windowPopout, far,
                   fitted ? "fitted to the window" : "the head's", fov[0], fov[1], fov[2], fov[3],
                   head.pos[0], head.pos[1], head.pos[2], s.winHead[2], head.tag, head.ipd);
        s.lastLogNs = now;
    }
    s.wasOn = true;
    return true;
}

// A demo starting: which one (the stage, the room, the running event's name), for the special scenes
// to come.
void logDemoStart() {
    dEvt_control_c* evt = g_dComIfG_gameInfo.play.getEvent();
    dEvent_manager_c& mng = g_dComIfG_gameInfo.play.getEvtManager();
    const s16 id = evt != nullptr ? evt->mEventId : -1;
    dEvDtEvent_c* data = id >= 0 ? mng.getEventData(id) : nullptr;
    char name[sizeof(data->mName) + 1] = "?";
    if (data != nullptr) {
        memcpy(name, data->mName, sizeof(data->mName));
        name[sizeof(data->mName)] = '\0';
    }
    const char* stage = dComIfGp_getStartStageName();
    pc::writef(STDERR_FILENO, "[vr] demo start: stage %s, room %d, event %d \"%s\", Helmaroc King %s\n",
               stage != nullptr ? stage : "?", dComIfGp_roomControl_getStayNo(), (int)id, name,
               fopAcM_SearchByName(fpcNm_DK_e) != nullptr || fopAcM_SearchByName(fpcNm_DEMO_DK_e) != nullptr ? "here"
                                                                                                          : "not here");
}

} // namespace

bool pc_vr_view_begin(view_class* view) {
    s.on = false;
    WqXrTable table;
    wq_xr_table(&table);
    WqXrHead head;
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    // the game's own first-person views, which the model cannot show: the giant screen meanwhile
    // (auto_screen): the telescope (the item, or the scope of an event), looking around, the picto box,
    // aiming the bow, the hookshot, the boomerang and the grappling hook
    // ... and the cutscenes (a demo playing, cine_screen), with the screen's 3D stronger (wq_xr.cpp)
    // (no demo manager in some scenes, as the title's: none playing)
    const bool cutscene = dComIfGp_demo_get() != nullptr && dComIfGp_demo_mode() != 0;
    wq_xr_set_cinematic(cutscene);
    static bool sDemoWas = false;
    if (cutscene && !sDemoWas) logDemoStart();
    sDemoWas = cutscene;
    // The special scenes (special_scenes): the Helmaroc King taking Aryll on Outset (a demo with the
    // bird, the d_a_dk or d_a_demo_dk actor). In the bubble (mode = maqueta), it closes in on the bird:
    // the pivot on it and the span times special_zoom, in and out over a second (updateFocus). With
    // special_window, through the window in any mode instead: the window's pose and size with a deep
    // camera and a large pop-out at the world's own size, so what comes at the camera comes out at the
    // player. Over cine_screen and auto_screen while it plays; the mode as it was after it.
    // (on Outset alone: the sea stage's room 44; the bird's other demos, in the Forsaken Fortress, stay
    // as they are)
    const char* stageName = dComIfGp_getStartStageName();
    fopAc_ac_c* bird = nullptr;
    if (cutscene && table.specialScenes && stageName != nullptr && strcmp(stageName, "sea") == 0 &&
        dComIfGp_roomControl_getStayNo() == 44) {
        bird = fopAcM_SearchByName(fpcNm_DEMO_DK_e);
        if (bird == nullptr) bird = fopAcM_SearchByName(fpcNm_DK_e);
    }
    const bool specialScene = bird != nullptr && (table.specialWindow || table.diorama);
    const bool special = specialScene && table.specialWindow;       // through the window
    const bool specialBubble = specialScene && !table.specialWindow;  // the bubble on the bird
    static int sSpecialWas = 0;
    const int specialNow = special ? 1 : specialBubble ? 2 : 0;
    if (specialNow != sSpecialWas) {
        pc::writef(STDERR_FILENO, "[vr] special%s\n",
                   special ? " scene on: the window, 3D for real"
                   : specialBubble ? ": bubble on the Helmaroc King" : " scene off");
        sSpecialWas = specialNow;
    }
    wq_xr_set_special(special);
    const bool firstPerson =
        table.autoScreen &&
        (dComIfGp_checkPlayerStatus0(0, daPyStts0_TELESCOPE_LOOK_e | daPyStts0_SUBJECT_e | daPyStts0_BOW_AIM_e |
                                            daPyStts0_HOOKSHOT_AIM_e | daPyStts0_BOOMERANG_AIM_e | daPyStts0_ROPE_AIM_e) != 0 ||
         dComIfGp_checkPlayerStatus1(0, daPyStts1_PICTO_BOX_AIM_e) != 0 || dComIfGp_getScopeType() != 0);
    const bool ownView = !specialScene && (firstPerson || (table.cineScreen && cutscene));
    const bool modeOn = table.diorama || table.window || special;
    const bool want = modeOn && view != nullptr && player != nullptr && !ownView && wq_xr_head(&head);
    if (!want) {
        if (s.wasOn) {
            pc::writef(STDERR_FILENO, "[vr] %s off (%s)\n", s.window ? "window" : "diorama",
                       !modeOn ? "mode" : view == nullptr ? "no camera" : player == nullptr ? "no Link"
                       : firstPerson ? "telescope or first person: the screen"
                       : ownView ? "a cutscene: the screen" : "no head pose");
            aurora_xr_set_frame_info(0, false);
            if (ownView || !modeOn) {
                // the screen's own 3D meanwhile (wq_xr.cpp), or after a special scene on the screen
                aurora_xr_set_eye_shift(false, 0.0f, 0.0f);
            }
            s.wasOn = false;
            s.rigValid = false;
        }
        s.window = false;
        s.tableValid = false;
        sBirds.n = 0;
        s.focusAmt = 0.0f;
        s.focusValid = false;
        wq_xr_set_talk(false);
        if (!modeOn) {
            ambientFrame(view, table, player);
        } else {
            ambientOff();
        }
        return false;
    }
    ambientOff();  // the diorama and the window: none
    // the game camera's heading on the model (stage space: the world's heading minus the model's turn),
    // for the left stick relative to the player's view (wq_xr.cpp)
    const bool freeControl = !pc::menuOpen() && dMenu_flag() == 0 && !dComIfGp_event_runCheck();
    const u8 mesg = dComIfGp_getMesgStatus();
    static u8 sMesgWas = 0;
    if (mesg != sMesgWas) {
        // when the dialogues' panel shows (with the "[cos] msg" lines, which message)
        pc::writef(STDERR_FILENO, "[vr] message status %u -> %u%s\n", (unsigned)sMesgWas, (unsigned)mesg,
                   dComIfGp_event_runCheck() ? " (event)" : "");
        sMesgWas = mesg;
    }
    if (table.window || special) {
        // the window: the stick as on the screen (the camera looks through the window)
        wq_xr_set_camera_heading(0.0f, false, mesg != 0);
        s.focusAmt = 0.0f;
        s.focusValid = false;
        wq_xr_set_talk(false);
        if (special) {
            WqXrTable w = table;
            w.windowScale = 1.0f;
            w.windowDepth = table.windowDepthSpecial;
            w.windowPopout = table.windowPopoutSpecial;
            return windowBegin(view, w, head, player);
        }
        return windowBegin(view, table, head, player);
    }
    if (s.window) {
        s.window = false;
        s.rigValid = false;  // from the window to the model: the rig starts again at Link
    }
    updateRig(player->current.pos, view->mViewMtx, table.followYaw);
    wq_xr_set_camera_heading(cameraYaw(view->mViewMtx) - s.yaw - table.yaw, freeControl, mesg != 0);

    // The talk zoom (talk_zoom_on): during a cutscene or while a message is open, the bubble closes in
    // on who talks, the span times talk_zoom: the middle between Link and the one he talks to, or, with
    // nobody, what the camera looks at (a cutscene's camera is the game's: its target), in and out over
    // a second (updateFocus); the game's 2D (the dialogue box) on a panel beside the bubble meanwhile
    // (talk_screen, wq_xr.cpp). The Helmaroc King's scene closes in on the bird instead (special_zoom),
    // the bubble's middle on it (the pivot 0.35 radii below the middle, tableFromStage).
    const bool talk = !specialBubble && table.talkZoom > 0.0f && (cutscene || mesg != 0);
    static bool sTalkWas = false;
    if (talk != sTalkWas) {
        pc::writef(STDERR_FILENO, "[vr] talk zoom %s\n", talk ? "on" : "off");
        sTalkWas = talk;
    }
    wq_xr_set_talk(talk);
    if (specialBubble) {
        const float zoomK = table.width / (table.span * table.specialZoom * kUnitsPerMetre);
        const float lift = table.sphere ? 0.35f * 0.5f * table.width / zoomK : 0.0f;
        const float target[3] = {bird->current.pos.x, bird->current.pos.y - lift, bird->current.pos.z};
        updateFocus(target, table.specialZoom);
    } else if (talk) {
        dEvt_control_c* evt = g_dComIfG_gameInfo.play.getEvent();
        fopAc_ac_c* partner = evt != nullptr ? evt->getPtT() : nullptr;
        float target[3];
        if (partner != nullptr && partner != player) {
            target[0] = 0.5f * (player->current.pos.x + partner->current.pos.x);
            target[1] = 0.5f * (player->current.pos.y + partner->current.pos.y);
            target[2] = 0.5f * (player->current.pos.z + partner->current.pos.z);
        } else {
            target[0] = view->mLookat.mCenter.x;
            target[1] = view->mLookat.mCenter.y;
            target[2] = view->mLookat.mCenter.z;
        }
        updateFocus(target, table.talkZoom);
    } else {
        updateFocus(nullptr, 1.0f);
    }
    {
        const float a = focusEase();
        for (int i = 0; i < 3; i++) s.view[i] = s.pivot[i] + (s.focus[i] - s.pivot[i]) * a;
        table.span *= 1.0f + (s.focusZoom - 1.0f) * a;
    }

    // the table: its half sizes, the cut below its top (a quarter of its smaller side), the scale
    s.sphere = table.sphere;
    s.halfX = 0.5f * table.width;
    s.halfZ = table.sphere ? s.halfX : 0.5f * table.depth;
    s.below = table.sphere ? s.halfX : 0.25f * std::min(table.width, table.depth);
    s.k = table.width / (table.span * kUnitsPerMetre);
    const M4 stage = stageFromWorld(table);
    const M4 tableStage = tableFromStage(table);
    s.tableFromWorld = mul(tableStage, stage);
    s.tableValid = true;
    s.cullViewValid = false;

    // C = V_head * Stage * World * V_game^-1: the game camera's view space to the head's (metres)
    const M4 c = mul(mul(headFromStage(head), stage), rigidInverse(view->mViewMtx));
    toMtx(c, s.delta);
    // the frustum both eyes are drawn with: the head's (no fit for the model)
    wq_xr_set_frame_view(head.tag, false, nullptr);
    Mtx44 p;
    C_MTXFrustum(p, kNear * head.tanUp, kNear * head.tanDown, kNear * head.tanLeft, kNear * head.tanRight, kNear, kFar);
    projParams(view->mProjMtx, s.projFrom);
    projParams(p, s.projTo);
    // each eye: the head moved by half the distance between the eyes along its x (the right eye +x):
    // clip.x shifts by -P00 * ipd / 2 for the right eye, the opposite for the left
    const float eyeShift = -p[0][0] * head.ipd * 0.5f;
    aurora_xr_set_eye_shift(true, 0.0f, eyeShift);
    // the eyes' mask (Aurora patch 0026): table from head, the edges' fade in table units
    {
        const M4 tableHead = mul(tableStage, stageFromHead(head));
        float tfh[12];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) tfh[i * 4 + j] = tableHead.m[i][j];
        const float r = 0.5f * table.width;
        float fade[4] = {table.fade / s.halfX, table.fade / s.halfZ, table.fade / s.below, 0.0f};
        if (table.sphere) fade[0] = fade[1] = fade[2] = table.fade / r, fade[3] = 1.0f;
        aurora_xr_set_diorama_mask(tfh, s.projTo, eyeShift, fade);
    }
    sBirds.n = 0;
    sBirds.range = table.birdRange;
    if (table.birdsFree) fopAcIt_Executor(collectBird, nullptr);
    aurora_xr_set_diorama_free(&sBirds.t[0][0], sBirds.n);
    aurora_xr_set_frame_info(head.tag, true);
    AuroraSetViewDelta(&s.delta[0][0], s.projFrom, s.projTo);
    seeThrough(mul(headFromStage(head), stage), player, table.seeThroughRadius, s.k);
    // the sky's things built around the camera's eye: squeezed to nothing (C = 0)
    memset(s.skyDelta, 0, sizeof(s.skyDelta));
    s.on = true;
    s.skyOn = false;

    const uint64_t now = nowNs();
    if (!s.wasOn || now - s.lastLogNs > 5000000000ull) {
        const float k = table.width / (table.span * kUnitsPerMetre);
        pc::writef(STDERR_FILENO,
                   "[vr] diorama: Link (%.0f, %.0f, %.0f), pivot (%.0f, %.0f, %.0f), heading %.0f deg, %.4f m per "
                   "unit; head (%.2f, %.2f, %.2f) tag %u, ipd %.3f\n",
                   player->current.pos.x, player->current.pos.y, player->current.pos.z, s.pivot[0], s.pivot[1],
                   s.pivot[2], s.yaw * 57.2958f, k, head.pos[0], head.pos[1], head.pos[2], head.tag, head.ipd);
        s.lastLogNs = now;
    }
    s.wasOn = true;
    return true;
}

void pc_vr_view_end(void) {
    if (s.on) {
        AuroraSetViewDelta(nullptr, nullptr, nullptr);
        s.on = false;
    }
    s.skyOn = false;
}

void pc_vr_sky_begin(void) {
    if (s.on && !s.skyOn) {
        AuroraSetViewDelta(&s.skyDelta[0][0], s.projFrom, s.projTo);
        s.skyOn = true;
    }
}

void pc_vr_sky_end(void) {
    if (s.on && s.skyOn) {
        AuroraSetViewDelta(&s.delta[0][0], s.projFrom, s.projTo);
    }
    s.skyOn = false;
}

void pc_vr_setting(const char* key, const char* value) { wq_xr_set(key, value); }
const char* pc_vr_perf_preset(void) { return wq_xr_perf_preset(); }
int pc_vr_resolution(int* base) { return wq_xr_resolution(base); }
const char* pc_vr_resolution_key(void) { return wq_xr_resolution_key(); }
void pc_vr_menu_state(bool open, float width, float height, bool hovered) {
    wq_xr_menu_state(open, width, height, hovered);
}

int pc_vr_panel_frame(void) {
    // the panel's game settings: their values for it, and its changes applied (and saved)
    for (const char* const* key = wq_xr_panel_game_keys(); *key != nullptr; key++) {
        const char* v = pc_settings_get(*key);
        wq_xr_panel_game_value(*key, v != nullptr ? v : "");
    }
    char key[64], value[64];
    while (wq_xr_panel_take_change(key, sizeof key, value, sizeof value)) {
        if (strcmp(key, "COS_VR_RESOLUTION") == 0) {
            pc_vr_apply_resolution(value);  // a preset's too, applied even when the row holds it
        } else {
            pc_settings_set(key, value);
        }
    }
    return wq_xr_panel_flags();
}

int pc_vr_clip_sphere(const f32 m[3][4], const Vec* center, f32 radius) {
    if (ambientClipActor()) return 0;
    if (!cullSetup()) return -1;
    Vec p;
    MTXMultVec(m, center, &p);
    const float v[3] = {p.x, p.y, p.z};
    return cullSphereView(v, radius);
}

int pc_vr_clip_box(const f32 m[3][4], const Vec* max, const Vec* min) {
    if (ambientClipActor()) return 0;
    if (!cullSetup()) return -1;
    // the box's middle and half diagonal, scaled as the matrix scales (its largest column)
    Vec c = {0.5f * (max->x + min->x), 0.5f * (max->y + min->y), 0.5f * (max->z + min->z)};
    const float dx = max->x - min->x, dy = max->y - min->y, dz = max->z - min->z;
    float scale = 0.0f;
    for (int col = 0; col < 3; col++)
        scale = std::max(scale, std::sqrt(m[0][col] * m[0][col] + m[1][col] * m[1][col] + m[2][col] * m[2][col]));
    Vec p;
    MTXMultVec(m, &c, &p);
    const float v[3] = {p.x, p.y, p.z};
    return cullSphereView(v, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) * scale);
}

int pc_vr_clip_model(J3DModel* model) {
    if (ambientClipActor()) {
        // every joint shown: its shapes may be out of the camera's frame and in the room
        J3DModelData* data = model->getModelData();
        for (u16 i = 0; i < data->getJointNum(); i++) {
            for (J3DMaterial* mat = data->getJointNodePointer(i)->getMesh(); mat != nullptr; mat = mat->getNext()) {
                mat->getShape()->show();
            }
        }
        return 0;
    }
    if (!cullSetup()) return -1;
    // as J3DUClipper::clipByBox, each joint's box against the table instead of the camera's view
    int hidden = 0;
    J3DModelData* data = model->getModelData();
    for (u16 i = 0; i < data->getJointNum(); i++) {
        J3DJoint* joint = data->getJointNodePointer(i);
        if (joint->getKind() != 0) continue;
        Mtx mtx;
        MTXConcat(j3dSys.getViewMtx(), model->getAnmMtx(i), mtx);
        const bool out = pc_vr_clip_box(mtx, joint->getMax(), joint->getMin()) == 1;
        for (J3DMaterial* mat = joint->getMesh(); mat != nullptr; mat = mat->getNext()) {
            if (out) {
                mat->getShape()->hide();
                hidden++;
            } else {
                mat->getShape()->show();
            }
        }
    }
    return hidden;
}

void pc_vr_ambient_record(void* matPacket, void* shapePacket) {
    if (sAmbient.recFrame != g_Counter.mCounter0) {
        sAmbient.recFrame = g_Counter.mCounter0;
        sAmbient.recN = 0;
    }
    if (sAmbient.recN < (int)(sizeof(sAmbient.rec) / sizeof(sAmbient.rec[0]))) {
        sAmbient.rec[sAmbient.recN++] = {(J3DMatPacket*)matPacket, (J3DShapePacket*)shapePacket};
    }
}

// After the world's lists and the particles: the ambient actor's packets again, marked for Aurora's
// ambience replays (patch 0034) and taken to the room by the ambience's view delta; then the paint's
// own view delta (60 fps's, or none) again. J3DMatPacket::draw's steps, for this packet's own shape
// (its list may hold another model's shapes merged into it).
void pc_vr_ambient_draw(void) {
    // the painter runs after the game frame's counter has moved on (fapGm_Execute): the packets taken
    // in this frame's actor draws are one count behind
    if (!sAmbient.on) return;
    sAmbient.paints++;
    if (sAmbient.recN == 0 || (u32)(g_Counter.mCounter0 - sAmbient.recFrame) > 1) {
        sAmbient.stale++;
        return;
    }
    view_class* view = dComIfGd_getView();
    if (view == nullptr) {
        sAmbient.noView++;
        return;
    }
    sAmbient.drawn++;
    GXSetProjection(view->mProjMtx, GX_PERSPECTIVE);
    AuroraSetViewDelta(&sAmbient.delta[0][0], sAmbient.projFrom, sAmbient.projTo);
    AuroraSetAmbient(true);
    j3dSys.reinitGX();
    for (int i = 0; i < sAmbient.recN; i++) {
        J3DMatPacket* mat = sAmbient.rec[i].mat;
        J3DShapePacket* shape = sAmbient.rec[i].shape;
        if (mat == nullptr || shape == nullptr || mat->getMaterial() == nullptr || shape->getShape() == nullptr) continue;
        j3dSys.setTexture(mat->getTexture());
        j3dSys.setMatPacket(mat);
        mat->getMaterial()->load();
        shape->getShape()->loadPreDrawSetting();
        if (shape->getDisplayListObj() != nullptr) shape->getDisplayListObj()->callDL();
        shape->drawFast();
        shape->getShape()->resetVcdVatCache();
    }
    AuroraSetAmbient(false);
    pc_fps60_view_restore();
}

#else
void pc_vr_ambient_record(void*, void*) {}
void pc_vr_ambient_draw(void) {}

bool pc_vr_view_begin(view_class*) { return false; }
void pc_vr_view_end(void) {}
void pc_vr_sky_begin(void) {}
void pc_vr_sky_end(void) {}
void pc_vr_setting(const char*, const char*) {}
const char* pc_vr_perf_preset(void) { return ""; }
int pc_vr_resolution(int*) { return -1; }
const char* pc_vr_resolution_key(void) { return "resolution"; }
void pc_vr_menu_state(bool, float, float, bool) {}
int pc_vr_panel_frame(void) { return 0; }
int pc_vr_clip_sphere(const f32[3][4], const Vec*, f32) { return -1; }
int pc_vr_clip_box(const f32[3][4], const Vec*, const Vec*) { return -1; }
int pc_vr_clip_model(J3DModel*) { return -1; }

#endif
