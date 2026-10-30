// The characters' draw distance (xr.ini npc_draw_distance, the headset panel's "Distancia de
// personajes"; Android only, every mode): the NPCs (an NPC_* process, or the NPC group) farther from
// Link than that are not drawn, as if the game's own culling had left them out (fopAc_Draw's culled
// branch). Several NPCs at once in view (Outset toward Grandma's house) made the frames stall in every
// mode. As with that culling, one with fopAcStts_NOCULLEXEC_e also pauses its execute while hidden
// (fopAc_Execute), as when the camera looks away; it goes on once Link is near again.
//
// - Only actors that accept the game's culling (fopAcStts_CULL_e) and never while an event runs (a
//   cutscene, a dialogue: the talk partner and every event actor are drawn) or for an actor the event
//   forces to move (dEvtMove_FORCE_e), nor in the game's first-person views (the telescope, the picto
//   box, looking around, aiming: what Link looks at from afar, the photos of the Nintendo Gallery).
// - Within kExecKeepUnits (30 m) a hidden NPC goes on running (2: drawn not, its execute kept): with a
//   short distance (down to 10 m) those that call Link or run off from some metres away, or walk a
//   route, keep doing so; farther, it pauses as when the camera looks away.
// - Hysteresis: hidden past the distance, drawn again within it less max(3 m, 8%), per actor (a table by
//   process ID), so one walking about the edge does not flicker.
// - No fade-in: an alpha or scale fade would go through each actor's own materials or its scale (game
//   logic); they appear as when the game's camera culling lets them in.
// - The title screen (introCinematic; fase 2: before, 1.0.8, a new file's first event on Outset, which
//   was not the one that stalled): its stage, sea_T (Outset behind the "Press Start" and the logo, the
//   camera flying about the island, most of it loaded, the frames slow). While it is the stage the NPCs
//   farther than kIntroUnits (40 m) from the game camera's eye are not drawn, event or not, Link or not,
//   their execute always kept; the vegetation's cut follows it (pc_veg_distance.cpp), and the headset
//   shows it on the flat giant screen (pc_vr_diorama.cpp, intro_2d). A narrower view than the game's
//   usual 60 degrees (a close-up from afar) takes those distances out as far as it zooms in. Over when
//   the stage changes (a file loaded, a new game).
#include "pc/pc_vr.h"
#include "pc_internal.h"

#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"
#include "f_op/f_op_view.h"

#if defined(__ANDROID__)
#include "wq_xr.h"
#endif

#include <cmath>
#include <cstring>
#include <unistd.h>

namespace {

constexpr int kSlots = 1024;  // a power of two
constexpr unsigned kStaleFrames = 90;
constexpr float kExecKeepUnits = 3000.0f;  // 30 m: a hidden NPC nearer than this keeps its execute

struct Slot {
    uint32_t id;
    uint32_t frame;  // last asked
    bool hidden;
};
Slot sSlots[kSlots];

float sDistUnits = 0.0f;  // 0: off
float sNear2 = 0.0f, sFar2 = 0.0f;
bool sEvent = false;  // last frame (the log)
bool sFirstPerson = false;  // last frame: a first-person view (all drawn)
unsigned sFrame = 0;
signed char sIsNpc[fpcNm_MAX_NUM_e];  // 0 unknown, 1 no, 2 an NPC_* process
unsigned sAsked = 0, sHidden = 0;
float sLastLogged = -1.0f;
uint64_t sLogFrame = 0;
// the title screen: its stage (sea_T) with a camera
constexpr float kIntroUnits = 4000.0f;
bool sIntro = false;
float sEye[3] = {0.0f, 0.0f, 0.0f};  // the game camera's eye, the last frame (the opening's centre)
float sZoom = 1.0f;                  // its zoom against a 60-degree view (1 at it or wider)

bool isNpc(fopAc_ac_c* ac) {
    if (fopAcM_GetGroup(ac) == fopAc_NPC_e) return true;
    const int name = fopAcM_GetName(ac);
    if (name < 0 || name >= fpcNm_MAX_NUM_e) return false;
    if (sIsNpc[name] == 0) sIsNpc[name] = strncmp(pc::censusProcName(name), "NPC_", 4) == 0 ? 2 : 1;
    return sIsNpc[name] == 2;
}

Slot* slotFor(uint32_t id) {
    uint32_t h = id * 2654435761u;
    Slot* free = nullptr;
    for (int i = 0; i < 16; i++) {
        Slot* sl = &sSlots[(h + i) & (kSlots - 1)];
        if (sl->frame != 0 && sl->id == id) return sl;
        if (free == nullptr && (sl->frame == 0 || sFrame - sl->frame > kStaleFrames)) free = sl;
    }
    if (free != nullptr) {
        free->id = id;
        free->hidden = false;
    }
    return free;
}

} // namespace

int pc_npc_distance_hidden(void* actor) {
    const fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (sIntro) {
        // the title: from the game camera, its execute kept (Link or not)
        fopAc_ac_c* ac = (fopAc_ac_c*)actor;
        if (ac == player || !fopAcM_CheckStatus(ac, fopAcStts_CULL_e) || !isNpc(ac)) return 0;
        const float dx = ac->current.pos.x - sEye[0], dy = ac->current.pos.y - sEye[1], dz = ac->current.pos.z - sEye[2];
        sAsked++;
        const float reach = kIntroUnits * sZoom;
        if (dx * dx + dy * dy + dz * dz <= reach * reach) return 0;
        sHidden++;
        return 2;
    }
    if (sDistUnits <= 0.0f || sFirstPerson || dComIfGp_event_runCheck()) return 0;
    if (player == nullptr) return 0;
    fopAc_ac_c* ac = (fopAc_ac_c*)actor;
    if (ac == player || !fopAcM_CheckStatus(ac, fopAcStts_CULL_e) || !isNpc(ac)) return 0;
    if (dComIfGp_event_moveApproval(ac) == dEvtMove_FORCE_e) return 0;
    const float dx = ac->current.pos.x - player->current.pos.x, dy = ac->current.pos.y - player->current.pos.y,
                dz = ac->current.pos.z - player->current.pos.z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    Slot* sl = slotFor(fopAcM_GetID(ac));
    bool hidden;
    if (sl == nullptr) {
        hidden = d2 > sFar2;
    } else {
        if (sl->frame == 0 || sFrame - sl->frame > kStaleFrames) sl->hidden = d2 > sFar2;  // new: no memory
        else if (sl->hidden) sl->hidden = d2 > sNear2;
        else sl->hidden = d2 > sFar2;
        sl->frame = sFrame;
        hidden = sl->hidden;
    }
    sAsked++;
    if (hidden) sHidden++;
    if (!hidden) return 0;
    return d2 <= kExecKeepUnits * kExecKeepUnits ? 2 : 1;
}

namespace pc {

bool drawDistanceAllDrawn() { return sEvent || sFirstPerson; }

bool introCinematic(float eye[3], float* zoom) {
    if (sIntro && eye != nullptr) memcpy(eye, sEye, sizeof(sEye));
    if (sIntro && zoom != nullptr) *zoom = sZoom;
    return sIntro;
}


void npcDistanceFrame(unsigned int frames) {
    // the last frame's counts, logged every 5 s with the census or at a change of the distance
    if (censusOn() && (frames - sLogFrame >= 150 || sLastLogged != sDistUnits) && (sAsked > 0 || sLastLogged != sDistUnits)) {
        writef(STDERR_FILENO, "[npc-distance] %.0f m: %u of %u NPCs not drawn%s\n", sDistUnits / 100.0f, sHidden,
               sAsked, sEvent ? " (an event runs: all drawn)" : sFirstPerson ? " (first person: all drawn)" : "");
        sLogFrame = frames;
        sLastLogged = sDistUnits;
    }
    sAsked = sHidden = 0;
    float metres = 0.0f;
#if defined(__ANDROID__)
    WqXrTable table;
    wq_xr_table(&table);
    metres = table.npcDrawDistance;
#endif
    sDistUnits = metres * 100.0f;
    const float nearUnits = sDistUnits - (sDistUnits * 0.08f > 300.0f ? sDistUnits * 0.08f : 300.0f);
    sFar2 = sDistUnits * sDistUnits;
    sNear2 = nearUnits > 0.0f ? nearUnits * nearUnits : 0.0f;
    sEvent = dComIfGp_event_runCheck();
    // the telescope (the item or an event's scope), looking around, the picto box, aiming (as
    // pc_vr_diorama.cpp's auto_screen): Link looks far, so everyone is drawn
    sFirstPerson = dComIfGp_getPlayer(0) != nullptr &&
                   (dComIfGp_checkPlayerStatus0(0, daPyStts0_TELESCOPE_LOOK_e | daPyStts0_SUBJECT_e | daPyStts0_BOW_AIM_e |
                                                       daPyStts0_HOOKSHOT_AIM_e | daPyStts0_BOOMERANG_AIM_e |
                                                       daPyStts0_ROPE_AIM_e) != 0 ||
                    dComIfGp_checkPlayerStatus1(0, daPyStts1_PICTO_BOX_AIM_e) != 0 || dComIfGp_getScopeType() != 0);
    sFrame = frames + 1;  // never 0 (a free slot)
#if defined(__ANDROID__)
    // the title screen: on while its stage (sea_T) is loaded and has a camera, the whole of it
    const view_class* view = dComIfGd_getView();
    if (view != nullptr) {
        sEye[0] = view->mLookat.mEye.x;
        sEye[1] = view->mLookat.mEye.y;
        sEye[2] = view->mLookat.mEye.z;
        const float t = std::tan(view->mFovy * 0.5f * 3.14159265f / 180.0f);
        sZoom = t > 1e-3f && t < 0.57735f ? 0.57735f / t : 1.0f;  // tan(30 degrees)
    }
    const char* stage = dComIfGp_getStartStageName();
    const bool title = view != nullptr && stage != nullptr && strcmp(stage, "sea_T") == 0;
    if (title != sIntro) {
        writef(STDERR_FILENO, "[npc-distance] the title screen (sea_T): %s\n",
               title ? "far NPCs and vegetation cut around the camera" : "over (left its stage)");
        sIntro = title;
    }
#endif
}

} // namespace pc
