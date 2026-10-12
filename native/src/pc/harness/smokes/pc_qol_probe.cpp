// COS_TRACE=qol: one line per game frame with what the quality-of-life options (pc_qol.h,
// docs/QOL_PLAN.md) change, for their Mac checks: run the same input with an option off and on and
// compare. Not a test by itself (no exit code); any boot or input script.
//   [cos] qol-probe f=<frame> <stage>:<room> pos=(x,y,z) spd=<speedF> ang=<shape_angle.y>
//     roll=<checkFrontRoll> ev=<event id> "<event name>" msg=<message number>:<status> eye=(x,y,z)
//     ship=<boat speedF, -1 without boat>@(<boat position>) do=<the A button's action, dActStts_*> sw=<switch, -1>
//     [act=<name>:(x,y,z)]
// Variables:
//   COS_QOL_PROBE_FROM=<frame>, COS_QOL_PROBE_TO=<frame>   only the lines of those frames;
//   COS_QOL_PROBE_ACTOR=<process name>   the first actor of that name (a block, the grappling hook);
//   COS_QOL_PROBE_SWITCH=<n>             the player's room's switch n;
//   COS_QOL_PROBE_SPAWN=<frame>:<process name>:<parameters>[:<distance>[:<angle z>]][;...] (up to 4)
//       creates that actor <distance> (150) in front of the player (angle z default 0xFFFF) at that
//       frame, from the player's layer; the first is the probed actor unless COS_QOL_PROBE_ACTOR says
//       otherwise;
//   COS_QOL_PROBE_MSG=<frame>:<message number>   shows that message (fopMsgM_messageSet) at that frame;
//   COS_QOL_PROBE_ROPE=<frame>   the player grabs the nearest hanging rope (d_a_himo3) as the rope smoke
//       does (pc_rope.cpp);
//   COS_QOL_PROBE_BALLAD=<frame>:<destination>   starts the boat's Ballad of Gales warp (a cyclone and
//       daShip_c::setTactWarp, as the song does) at that frame; the player must be on the boat;
//   COS_QOL_PROBE_WARP=<frame>:<stage>:<room>:<point>   the options menu's travel at that frame
//       (pc_menu.cpp doWarp: dComIfGp_setNextStage with the game's own layer);
//   COS_QOL_PROBE_GIVE=<frame>:<name>[,<name>...]   the debug server's "give" at that frame (pc_debug_give,
//       pc_menu.cpp: the same queue, run by the game thread at the frame's end);
//   COS_QOL_PROBE_SE=<n>   every n frames (also outside the PLAY scene) one line with what JAudio plays:
//       "[cos] qol-probe se f=<frame> <pc_audio_state.cpp's line>" (the registered sound effects, the
//       SE tracks still sounding, the BGM, the audio thread's counters).
#include "pc_internal.h"

#include "d/actor/d_a_player.h"
#include "d/actor/d_a_ship.h"
#include "d/d_com_inf_game.h"
#include "d/d_event_data.h"
#include "d/d_event_manager.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_camera.h"
#include "f_op/f_op_msg.h"
#include "f_op/f_op_msg_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"

#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace pc {

namespace {

int sOn = -1;
int sActorName = -1;
unsigned int sFrom = 0;
unsigned int sTo = ~0u;
unsigned int sMsgFrame = 0;
unsigned int sMsgNo = 0;
fpc_ProcID sMsgId = fpcM_ERROR_PROCESS_ID_e;
unsigned int sRopeFrame = 0;
unsigned int sBalladFrame = 0;
int sBalladTo = 0;
struct Spawn {
    unsigned int frame;
    int name;
    u32 param;
    float distance;
    s16 angleZ;
};
Spawn sSpawns[4];
int sSpawnCount = 0;
int sSwitch = -1;
unsigned int sWarpFrame = 0;
char sWarpStage[8] = {};
int sWarpRoom = 0;
int sWarpPoint = 0;
unsigned int sSeEvery = 0;
unsigned int sGiveFrame = 0;
char sGive[256] = {};

// What JAudio plays (pc_audio_state.cpp).
void seLine(unsigned int frames) {
    char line[1536];
    audioStateLine(line, sizeof(line));
    writef(STDERR_FILENO, "[cos] qol-probe se f=%u %s\n", frames, line);
}

void* isPlayScene(void* proc, void*) {
    return fpcM_GetName(proc) == fpcNm_PLAY_SCENE_e ? proc : nullptr;
}

void* isActorName(void* proc, void*) {
    return fpcM_IsCreating(fpcM_GetID(proc)) == FALSE && fpcM_GetName(proc) == sActorName ? proc : nullptr;
}

void* nearestRope(void* proc, void* data) {
    fopAc_ac_c** best = (fopAc_ac_c**)data;
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (fopAcM_GetName((fopAc_ac_c*)proc) == fpcNm_HIMO3_e) {
        fopAc_ac_c* rope = (fopAc_ac_c*)proc;
        if (*best == nullptr ||
            (rope->current.pos - player->current.pos).absXZ() < ((*best)->current.pos - player->current.pos).absXZ()) {
            *best = rope;
        }
    }
    return nullptr;
}

void* isMessage(void* proc, void*) {
    const s16 name = fpcM_GetName(proc);
    return name == fpcNm_MSG_e || name == fpcNm_MSG2_e || name == fpcNm_MSG3_e ? proc : nullptr;
}

} // namespace

} // namespace pc

extern "C" int pc_debug_grab_rope(fopAc_ac_c* rope, const cXyz* at); // pc_rope.cpp
extern "C" int pc_debug_give(const char* const* names, int count, char* done, size_t size); // pc_menu.cpp

namespace pc {

void qolProbeFrame(unsigned int frames) {
    if (sOn < 0) {
        sOn = pc_trace_enabled("qol");
        if (const char* v = getenv("COS_QOL_PROBE_ACTOR"); v != nullptr) {
            sActorName = (int)strtol(v, nullptr, 0);
        }
        if (const char* v = getenv("COS_QOL_PROBE_FROM"); v != nullptr) {
            sFrom = (unsigned int)strtoul(v, nullptr, 0);
        }
        if (const char* v = getenv("COS_QOL_PROBE_TO"); v != nullptr) {
            sTo = (unsigned int)strtoul(v, nullptr, 0);
        }
        for (const char* v = getenv("COS_QOL_PROBE_SPAWN"); v != nullptr && *v != '\0' && sSpawnCount < 4;) {
            Spawn s = {0, -1, 0, 150.0f, (s16)0xFFFF};
            char* end = nullptr;
            s.frame = (unsigned int)strtoul(v, &end, 0);
            if (*end == ':') {
                s.name = (int)strtol(end + 1, &end, 0);
            }
            if (*end == ':') {
                s.param = (u32)strtoul(end + 1, &end, 0);
            }
            if (*end == ':') {
                s.distance = strtof(end + 1, &end);
            }
            if (*end == ':') {
                s.angleZ = (s16)strtol(end + 1, &end, 0);
            }
            sSpawns[sSpawnCount++] = s;
            if (sActorName < 0) {
                sActorName = s.name;
            }
            v = *end == ';' ? end + 1 : nullptr;
        }
        if (const char* v = getenv("COS_QOL_PROBE_SWITCH"); v != nullptr) {
            sSwitch = (int)strtol(v, nullptr, 0);
        }
        if (const char* v = getenv("COS_QOL_PROBE_ROPE"); v != nullptr) {
            sRopeFrame = (unsigned int)strtoul(v, nullptr, 0);
        }
        if (const char* v = getenv("COS_QOL_PROBE_BALLAD"); v != nullptr) {
            char* end = nullptr;
            sBalladFrame = (unsigned int)strtoul(v, &end, 0);
            sBalladTo = end != nullptr && *end == ':' ? (int)strtol(end + 1, nullptr, 0) : 0;
        }
        if (const char* v = getenv("COS_QOL_PROBE_WARP"); v != nullptr) {
            char stage[16] = {};
            unsigned int f = 0;
            if (sscanf(v, "%u:%7[^:]:%d:%d", &f, stage, &sWarpRoom, &sWarpPoint) == 4) {
                sWarpFrame = f;
                snprintf(sWarpStage, sizeof(sWarpStage), "%s", stage);
            }
        }
        if (const char* v = getenv("COS_QOL_PROBE_GIVE"); v != nullptr) {
            char* end = nullptr;
            sGiveFrame = (unsigned int)strtoul(v, &end, 0);
            if (end != nullptr && *end == ':') {
                snprintf(sGive, sizeof(sGive), "%s", end + 1);
            }
        }
        if (const char* v = getenv("COS_QOL_PROBE_SE"); v != nullptr) {
            sSeEvery = (unsigned int)strtoul(v, nullptr, 0);
        }
        if (const char* v = getenv("COS_QOL_PROBE_MSG"); v != nullptr) {
            char* end = nullptr;
            sMsgFrame = (unsigned int)strtoul(v, &end, 0);
            sMsgNo = end != nullptr && *end == ':' ? (unsigned int)strtoul(end + 1, nullptr, 0) : 0;
        }
    }
    if (sOn == 0 || frames < sFrom || frames > sTo) {
        return;
    }
    if (sSeEvery != 0 && frames % sSeEvery == 0) {
        seLine(frames);
    }
    if (fpcM_Search(isPlayScene, nullptr) == nullptr) {
        return;
    }
    daPy_py_c* player = (daPy_py_c*)dComIfGp_getPlayer(0);
    if (player == nullptr || fopAcM_GetName(player) != fpcNm_PLAYER_e || fpcM_IsCreating(fopAcM_GetID(player))) {
        return;
    }

    // Made from the player's layer, as the player's own requests are (pc_actor_sweep.cpp).
    layer_class* savedLayer = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(static_cast<base_process_class*>(player)->mLyTg.mpLayer);
    for (int i = 0; i < sSpawnCount; i++) {
        const Spawn& s = sSpawns[i];
        if (s.name < 0 || frames != s.frame) {
            continue;
        }
        const s16 yaw = player->shape_angle.y;
        cXyz pos = player->current.pos;
        pos.x += s.distance * cM_ssin(yaw);
        pos.z += s.distance * cM_scos(yaw);
        csXyz angle(0, (s16)(yaw + 0x8000), s.angleZ);
        fpc_ProcID id = fopAcM_create((s16)s.name, s.param, &pos, fopAcM_GetRoomNo(player), &angle);
        writef(STDERR_FILENO, "[cos] qol-probe: actor %d (0x%08X) spawned at frame %u at (%.1f,%.1f,%.1f) (process %d)\n",
               s.name, s.param, frames, pos.x, pos.y, pos.z, (int)id);
    }
    if (sRopeFrame != 0 && frames == sRopeFrame) {
        // The rope smoke's grab (pc_rope.cpp): the nearest hanging rope, 200 below its top.
        fopAc_ac_c* rope = nullptr;
        fpcM_Search(nearestRope, &rope);
        if (rope != nullptr) {
            cXyz at(rope->current.pos.x + 20.0f, rope->current.pos.y - 200.0f, rope->current.pos.z);
            pc_debug_grab_rope(rope, &at);
            writef(STDERR_FILENO, "[cos] qol-probe: rope grabbed at frame %u\n", frames);
        }
    }
    if (sBalladFrame != 0 && frames == sBalladFrame) {
        // What the Ballad of Gales does on the boat once a destination is chosen
        // (daPy_lk_c::procTactPlay_init, procTactPlayEnd): the cyclone, then the boat's warp.
        if (daShip_c* boat = dComIfGp_getShipActor(); boat != nullptr) {
            fpc_ProcID warpID = fopAcM_create(fpcNm_TORNADO_e, 1, &player->current.pos, fopAcM_GetRoomNo(player));
            boat->setTactWarpID(warpID);
            boat->setTactWarpPosNum(sBalladTo);
            boat->setTactWarp();
            writef(STDERR_FILENO, "[cos] qol-probe: Ballad of Gales warp %d ordered at frame %u\n", sBalladTo, frames);
        }
    }
    if (sGiveFrame != 0 && frames == sGiveFrame && sGive[0] != '\0') {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", sGive);
        const char* names[16];
        int count = 0;
        for (char* t = strtok(buf, ","); t != nullptr && count < 16; t = strtok(nullptr, ",")) {
            names[count++] = t;
        }
        char done[512];
        const int ok = pc_debug_give(names, count, done, sizeof(done));
        writef(STDERR_FILENO, "[cos] qol-probe: give %s at frame %u: %s\n", sGive, frames, ok ? done : "unknown item");
    }
    if (sWarpFrame != 0 && frames == sWarpFrame) {
        writef(STDERR_FILENO, "[cos] qol-probe: warp to %s room %d point %d at frame %u\n", sWarpStage, sWarpRoom,
               sWarpPoint, frames);
        dComIfGp_setNextStage(sWarpStage, (s16)sWarpPoint, (s8)sWarpRoom, -1);
    }
    if (sMsgNo != 0 && frames == sMsgFrame) {
        sMsgId = fopMsgM_messageSet(sMsgNo, &player->eyePos);
        writef(STDERR_FILENO, "[cos] qol-probe: message %u shown at frame %u (process %d)\n", sMsgNo, frames,
               (int)sMsgId);
    }
    fpcLy_SetCurrentLayer(savedLayer);

    dEvt_control_c* ev = g_dComIfG_gameInfo.play.getEvent();
    const char* evName = "";
    if (ev->mEventId != -1) {
        dEvDtEvent_c* data = dComIfGp_getPEvtManager()->getEventData(ev->mEventId);
        if (data != nullptr) {
            evName = data->getName();
        }
    }
    // The first message process (d_msg.cpp and the others): its message number and status (fopMsgStts_*), 0 without one.
    unsigned int msgNo = 0, msgStatus = 0;
    msg_class* msg = sMsgId != fpcM_ERROR_PROCESS_ID_e ? fopMsgM_SearchByID(sMsgId) : nullptr;
    if (msg == nullptr) {
        msg = (msg_class*)fpcM_Search(isMessage, nullptr);
    }
    if (msg != nullptr) {
        msgNo = msg->mMsgNo;
        msgStatus = msg->mStatus;
    }
    cXyz eye(0.0f, 0.0f, 0.0f);
    if (camera_process_class* cam = dComIfGp_getCamera(0); cam != nullptr) {
        eye = cam->view.mLookat.mEye;
    }
    daShip_c* ship = dComIfGp_getShipActor();
    char act[96] = "";
    if (sActorName >= 0) {
        if (fopAc_ac_c* a = (fopAc_ac_c*)fpcM_Search(isActorName, nullptr); a != nullptr) {
            snprintf(act, sizeof(act), " act=%d:(%.1f,%.1f,%.1f)", sActorName, a->current.pos.x, a->current.pos.y,
                     a->current.pos.z);
        } else {
            snprintf(act, sizeof(act), " act=%d:-", sActorName);
        }
    }
    writef(STDERR_FILENO,
           "[cos] qol-probe f=%u %s:%d pos=(%.1f,%.1f,%.1f) spd=%.2f ang=%04x roll=%d ev=%d \"%s\" msg=%u:%u "
           "eye=(%.1f,%.1f,%.1f) ship=%.2f@(%.0f,%.0f,%.0f) do=%u sw=%d%s\n",
           frames, dComIfGp_getStartStageName(), (int)fopAcM_GetRoomNo(player), player->current.pos.x,
           player->current.pos.y, player->current.pos.z, player->speedF, (unsigned)(u16)player->shape_angle.y,
           player->checkFrontRoll() ? 1 : 0, ev->mEventId, evName, msgNo, msgStatus, eye.x, eye.y,
           eye.z, ship != nullptr ? ship->speedF : -1.0f, ship != nullptr ? ship->current.pos.x : 0.0f,
           ship != nullptr ? ship->current.pos.y : 0.0f, ship != nullptr ? ship->current.pos.z : 0.0f,
           (unsigned)dComIfGp_getDoStatus(),
           sSwitch >= 0 ? (int)dComIfGs_isSwitch(sSwitch, fopAcM_GetRoomNo(player)) : -1, act);
}

} // namespace pc
