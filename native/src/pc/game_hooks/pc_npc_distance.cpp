// The characters' draw distance (xr.ini npc_draw_distance, the headset panel's "Distancia de
// personajes"; Android only, every mode): the NPCs (an NPC_* process, or the NPC group) farther from
// Link than that are not drawn, as if the game's own culling had left them out (fopAc_Draw's culled
// branch). Several NPCs at once in view (Outset toward Grandma's house) made the frames stall in every
// mode. As with that culling, one with fopAcStts_NOCULLEXEC_e also pauses its execute while hidden
// (fopAc_Execute), as when the camera looks away; it goes on once Link is near again.
//
// - Only actors that accept the game's culling (fopAcStts_CULL_e) and never while an event runs (a
//   cutscene, a dialogue: the talk partner and every event actor are drawn) or for an actor the event
//   forces to move (dEvtMove_FORCE_e).
// - Hysteresis: hidden past the distance, drawn again within it less max(3 m, 8%), per actor (a table by
//   process ID), so one walking about the edge does not flicker.
// - No fade-in: an alpha or scale fade would go through each actor's own materials or its scale (game
//   logic); they appear as when the game's camera culling lets them in.
#include "pc/pc_vr.h"
#include "pc_internal.h"

#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#if defined(__ANDROID__)
#include "wq_xr.h"
#endif

#include <cstring>
#include <unistd.h>

namespace {

constexpr int kSlots = 1024;  // a power of two
constexpr unsigned kStaleFrames = 90;

struct Slot {
    uint32_t id;
    uint32_t frame;  // last asked
    bool hidden;
};
Slot sSlots[kSlots];

float sDistUnits = 0.0f;  // 0: off
float sNear2 = 0.0f, sFar2 = 0.0f;
bool sEvent = false;  // last frame (the log)
unsigned sFrame = 0;
signed char sIsNpc[fpcNm_MAX_NUM_e];  // 0 unknown, 1 no, 2 an NPC_* process
unsigned sAsked = 0, sHidden = 0;
float sLastLogged = -1.0f;
uint64_t sLogFrame = 0;

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
    if (sDistUnits <= 0.0f || dComIfGp_event_runCheck()) return 0;
    const fopAc_ac_c* player = dComIfGp_getPlayer(0);
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
    return hidden ? 1 : 0;
}

namespace pc {

void npcDistanceFrame(unsigned int frames) {
    // the last frame's counts, logged every 5 s with the census or at a change of the distance
    if (censusOn() && (frames - sLogFrame >= 150 || sLastLogged != sDistUnits) && (sAsked > 0 || sLastLogged != sDistUnits)) {
        writef(STDERR_FILENO, "[npc-distance] %.0f m: %u of %u NPCs not drawn%s\n", sDistUnits / 100.0f, sHidden,
               sAsked, sEvent ? " (an event runs: all drawn)" : "");
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
    sFrame = frames + 1;  // never 0 (a free slot)
}

} // namespace pc
