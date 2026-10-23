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
// Phase 1: no cut at the table's edges and no culling from the head yet; the HUD stays in the picture.
#include "d/dolzel.h" // IWYU pragma: keep
#include "pc/pc_vr.h"

#if defined(__ANDROID__)

#include "pc/pc_harness.h"
#include "pc_internal.h"

#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_view.h"

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

// Stage from world: the world's axes scaled to metres, the pivot at the table's middle.
M4 stageFromWorld(const WqXrTable& t) {
    float r[3], u[3], b[3];
    axes(s.yaw, r, u, b);
    const float k = t.width / (t.span * kUnitsPerMetre);  // metres per game unit
    M4 m = identity();
    for (int c = 0; c < 3; c++) {
        m.m[0][c] = r[c] * k;
        m.m[1][c] = u[c] * k;
        m.m[2][c] = b[c] * k;
    }
    const float anchor[3] = {0.0f, -t.height, -t.distance};
    for (int row = 0; row < 3; row++)
        m.m[row][3] = anchor[row] - (m.m[row][0] * s.pivot[0] + m.m[row][1] * s.pivot[1] + m.m[row][2] * s.pivot[2]);
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

} // namespace

bool pc_vr_view_begin(view_class* view) {
    s.on = false;
    WqXrTable table;
    wq_xr_table(&table);
    WqXrHead head;
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const bool want = table.diorama && view != nullptr && player != nullptr && wq_xr_head(&head);
    if (!want) {
        if (s.wasOn) {
            pc::writef(STDERR_FILENO, "[vr] diorama off (%s)\n",
                       !table.diorama ? "mode" : view == nullptr ? "no camera" : player == nullptr ? "no Link" : "no head pose");
            aurora_xr_set_frame_info(0, false);
            s.wasOn = false;
            s.rigValid = false;
        }
        return false;
    }
    updateRig(player->current.pos, view->mViewMtx, table.followYaw);

    // C = V_head * Stage * World * V_game^-1: the game camera's view space to the head's (metres)
    const M4 c = mul(mul(headFromStage(head), stageFromWorld(table)), rigidInverse(view->mViewMtx));
    toMtx(c, s.delta);
    // the frustum both eyes are drawn with
    Mtx44 p;
    C_MTXFrustum(p, kNear * head.tanUp, kNear * head.tanDown, kNear * head.tanLeft, kNear * head.tanRight, kNear, kFar);
    projParams(view->mProjMtx, s.projFrom);
    projParams(p, s.projTo);
    // each eye: the head moved by half the distance between the eyes along its x (the right eye +x):
    // clip.x shifts by -P00 * ipd / 2 for the right eye, the opposite for the left
    aurora_xr_set_eye_shift(true, 0.0f, -p[0][0] * head.ipd * 0.5f);
    aurora_xr_set_frame_info(head.tag, true);
    AuroraSetViewDelta(&s.delta[0][0], s.projFrom, s.projTo);
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

#else

bool pc_vr_view_begin(view_class*) { return false; }
void pc_vr_view_end(void) {}
void pc_vr_sky_begin(void) {}
void pc_vr_sky_end(void) {}

#endif
