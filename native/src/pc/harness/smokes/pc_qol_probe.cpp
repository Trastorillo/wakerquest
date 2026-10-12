// COS_TRACE=qol: one line per game frame with what the quality-of-life options (pc_qol.h,
// docs/QOL_PLAN.md) change, for their Mac checks: run the same input with an option off and on and
// compare. Not a test by itself (no exit code); any boot or input script.
//   [cos] qol-probe f=<frame> <stage>:<room> pos=(x,y,z) spd=<speedF> ang=<shape_angle.y>
//     roll=<checkFrontRoll> ev=<event id> "<event name>" msg=<talk message number>:<status> eye=(x,y,z)
//     ship=<boat speedF, -1 without boat> do=<the A button's action, dActStts_*> [act=<name>:(x,y,z)]
// COS_QOL_PROBE_ACTOR=<process name number> adds the first actor of that name (e.g. a block or the
// grappling hook) to each line. COS_QOL_PROBE_MSG=<frame>:<message number> shows that message
// (fopMsgM_messageSet, as a talk does) at that frame. COS_QOL_PROBE_SPAWN=<frame>:<process name>:
// <parameters>[:<distance>] creates that actor (angle z 0xFFFF) <distance> (150) in front of the
// player at that frame, and is the probed actor unless COS_QOL_PROBE_ACTOR says otherwise.
// COS_QOL_PROBE_BALLAD=<frame>:<destination> starts the boat's Ballad of Gales warp (a cyclone and
// daShip_c::setTactWarp, as the song does) at that frame; the player must be on the boat. COS_QOL_PROBE_FROM=<frame> / COS_QOL_PROBE_TO=<frame> limit the lines.
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
unsigned int sBalladFrame = 0;
int sBalladTo = 0;
unsigned int sSpawnFrame = 0;
int sSpawnName = -1;
u32 sSpawnParam = 0;
float sSpawnDistance = 150.0f;

void* isPlayScene(void* proc, void*) {
    return fpcM_GetName(proc) == fpcNm_PLAY_SCENE_e ? proc : nullptr;
}

void* isActorName(void* proc, void*) {
    return fpcM_IsCreating(fpcM_GetID(proc)) == FALSE && fpcM_GetName(proc) == sActorName ? proc : nullptr;
}

void* isMessage(void* proc, void*) {
    const s16 name = fpcM_GetName(proc);
    return name == fpcNm_MSG_e || name == fpcNm_MSG2_e || name == fpcNm_MSG3_e ? proc : nullptr;
}

} // namespace

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
        if (const char* v = getenv("COS_QOL_PROBE_SPAWN"); v != nullptr) {
            char* end = nullptr;
            sSpawnFrame = (unsigned int)strtoul(v, &end, 0);
            if (end != nullptr && *end == ':') {
                sSpawnName = (int)strtol(end + 1, &end, 0);
            }
            if (end != nullptr && *end == ':') {
                sSpawnParam = (u32)strtoul(end + 1, &end, 0);
            }
            if (end != nullptr && *end == ':') {
                sSpawnDistance = strtof(end + 1, &end);
            }
            if (sActorName < 0) {
                sActorName = sSpawnName;
            }
        }
        if (const char* v = getenv("COS_QOL_PROBE_BALLAD"); v != nullptr) {
            char* end = nullptr;
            sBalladFrame = (unsigned int)strtoul(v, &end, 0);
            sBalladTo = end != nullptr && *end == ':' ? (int)strtol(end + 1, nullptr, 0) : 0;
        }
        if (const char* v = getenv("COS_QOL_PROBE_MSG"); v != nullptr) {
            char* end = nullptr;
            sMsgFrame = (unsigned int)strtoul(v, &end, 0);
            sMsgNo = end != nullptr && *end == ':' ? (unsigned int)strtoul(end + 1, nullptr, 0) : 0;
        }
    }
    if (sOn == 0 || frames < sFrom || frames > sTo || fpcM_Search(isPlayScene, nullptr) == nullptr) {
        return;
    }
    daPy_py_c* player = (daPy_py_c*)dComIfGp_getPlayer(0);
    if (player == nullptr || fopAcM_GetName(player) != fpcNm_PLAYER_e || fpcM_IsCreating(fopAcM_GetID(player))) {
        return;
    }

    // Made from the player's layer, as the player's own requests are (pc_actor_sweep.cpp).
    layer_class* savedLayer = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(static_cast<base_process_class*>(player)->mLyTg.mpLayer);
    if (sSpawnName >= 0 && frames == sSpawnFrame) {
        const s16 yaw = player->shape_angle.y;
        cXyz pos = player->current.pos;
        pos.x += sSpawnDistance * cM_ssin(yaw);
        pos.z += sSpawnDistance * cM_scos(yaw);
        csXyz angle(0, (s16)(yaw + 0x8000), (s16)0xFFFF);
        fpc_ProcID id = fopAcM_create((s16)sSpawnName, sSpawnParam, &pos, fopAcM_GetRoomNo(player), &angle);
        writef(STDERR_FILENO, "[cos] qol-probe: actor %d (0x%08X) spawned at frame %u at (%.1f,%.1f,%.1f) (process %d)\n",
               sSpawnName, sSpawnParam, frames, pos.x, pos.y, pos.z, (int)id);
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
           "eye=(%.1f,%.1f,%.1f) ship=%.2f do=%u%s\n",
           frames, dComIfGp_getStartStageName(), (int)fopAcM_GetRoomNo(player), player->current.pos.x,
           player->current.pos.y, player->current.pos.z, player->speedF, (unsigned)(u16)player->shape_angle.y,
           player->checkFrontRoll() ? 1 : 0, ev->mEventId, evName, msgNo, msgStatus, eye.x, eye.y,
           eye.z, ship != nullptr ? ship->speedF : -1.0f, (unsigned)dComIfGp_getDoStatus(), act);
}

} // namespace pc
