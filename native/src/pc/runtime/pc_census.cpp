// The ambient census (COS_CENSUS, logging only): every 5 s, and once at each stage or room change,
// one line per room with the actors alive in it by process name (f_pc_name.h) and count, and on the
// room Link stands in the particle emitters alive by JPA user index (and resource manager when it is
// not the common one) and the weather's own GX draws (rain, snow, spores, mist). For a later
// "ambient in the room": which seagulls, birds, crabs, fireflies, wind lines... a stage keeps around.
// Read-only, at the end of the game frame (pc_frame_end, after the frame's execute and draw); no
// allocation (static tables).
//
// COS_CENSUS: off unless "1" or "on" (a diagnostic: player builds ship it off; on, it costs one actor
// walk and one emitter walk every 5 s). The window's line ("[census] window: ...", pc_vr_diorama.cpp)
// and the characters' draw distance line (pc_npc_distance.cpp) use the same switch.
#include "pc_internal.h"
#include "pc/pc_vr.h"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_particle.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "JSystem/JParticle/JPAEmitterManager.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <unistd.h>

const void* pc_vr_clip_actor = nullptr;
const char* pc_vr_clip_kind = nullptr;
const void* pc_vr_ambient_actor = nullptr;  // pc_vr.h: the room's ambience (pc_vr_diorama.cpp)
int pc_vr_ambient_jpa = 0;

namespace pc {

namespace {

struct ProcName {
    int id;
    const char* name;
};

const ProcName kProcNames[] = {
#include "pc_proc_names.inc"
};

const char* sNames[fpcNm_MAX_NUM_e];
bool sNamesReady = false;

constexpr int kRooms = 65;  // room -1 (the stage's own) .. 63
constexpr int kTop = 40;
constexpr int kMaxEmitterKinds = 512;
constexpr uint64_t kPeriodNs = 5000000000ull;

uint16_t sCount[kRooms][fpcNm_MAX_NUM_e];
uint16_t sRoomTotal[kRooms];
uint32_t sEmKey[kMaxEmitterKinds];
uint16_t sEmCount[kMaxEmitterKinds];
int sEmKinds = 0;

int sOn = -1;
uint64_t sLastNs = 0;
char sStage[16] = "";
int sStay = -100;
char sLine[2048];

uint64_t nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int countActor(void* p, void*) {
    fopAc_ac_c* ac = (fopAc_ac_c*)p;
    const int name = fopAcM_GetName(ac);
    if (name < 0 || name >= fpcNm_MAX_NUM_e) return 1;
    int room = fopAcM_GetRoomNo(ac) + 1;
    if (room < 0 || room >= kRooms) room = 0;
    if (sCount[room][name] != 0xFFFF) sCount[room][name]++;
    sRoomTotal[room]++;
    return 1;
}

void countEmitters() {
    sEmKinds = 0;
    JPAEmitterManager* mng = dPa_control_c::getEmitterManager();
    if (mng == nullptr) return;
    for (int g = 0; g < 16; g++) {
        for (JSULink<JPABaseEmitter>* l = mng->mEmtrGroup[g].getFirst(); l != nullptr; l = l->getNext()) {
            JPABaseEmitter* e = l->getObject();
            const uint32_t key = ((uint32_t)e->getResourceManagerID() << 16) | e->getPcUserID();
            int i = 0;
            while (i < sEmKinds && sEmKey[i] != key) i++;
            if (i == sEmKinds) {
                if (sEmKinds >= kMaxEmitterKinds) continue;
                sEmKey[sEmKinds] = key;
                sEmCount[sEmKinds++] = 0;
            }
            sEmCount[i]++;
        }
    }
}

// appends to sLine at *len, bounded
void put(size_t* len, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void put(size_t* len, const char* fmt, ...) {
    if (*len >= sizeof(sLine) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(sLine + *len, sizeof(sLine) - *len, fmt, ap);
    va_end(ap);
    if (n > 0) *len = std::min(*len + (size_t)n, sizeof(sLine) - 1);
}

void putActors(size_t* len, int room) {
    // top kTop by count: repeated pick of the largest not yet printed (marked by clearing it)
    uint16_t* c = sCount[room];
    put(len, "actors %u:", (unsigned)sRoomTotal[room]);
    int printed = 0, rest = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < fpcNm_MAX_NUM_e; i++)
            if (c[i] != 0 && (best < 0 || c[i] > c[best])) best = i;
        if (best < 0) break;
        if (printed < kTop) {
            put(len, "%s %sx%u", printed == 0 ? "" : ",", censusProcName(best), (unsigned)c[best]);
            printed++;
        } else {
            rest++;
        }
        c[best] = 0;
    }
    if (rest > 0) put(len, ", +%d kinds", rest);
}

void putEmitters(size_t* len) {
    JPAEmitterManager* mng = dPa_control_c::getEmitterManager();
    put(len, "; emitters %u:", mng != nullptr ? (unsigned)mng->getEmitterNumber() : 0u);
    int printed = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < sEmKinds; i++)
            if (sEmCount[i] != 0 && (best < 0 || sEmCount[i] > sEmCount[best])) best = i;
        if (best < 0 || printed >= kTop) break;
        const unsigned rm = sEmKey[best] >> 16, id = sEmKey[best] & 0xFFFF;
        if (rm == 0)
            put(len, "%s %04xx%u", printed == 0 ? "" : ",", id, (unsigned)sEmCount[best]);
        else
            put(len, "%s %u:%04xx%u", printed == 0 ? "" : ",", rm, id, (unsigned)sEmCount[best]);
        sEmCount[best] = 0;
        printed++;
    }
    if (printed == 0) put(len, " -");
    // the weather's GX draws (d_kankyo_rain.cpp dKyr_draw*), not emitters
    put(len, "; kankyo rain %d snow %d housi %d moya %d", g_env_light.mRainCount, g_env_light.mSnowCount,
        g_env_light.mHousiCount, (int)g_env_light.mMoyaCount);
}

void report(const char* why, unsigned int frames) {
    memset(sCount, 0, sizeof(sCount));
    memset(sRoomTotal, 0, sizeof(sRoomTotal));
    fopAcIt_Executor(countActor, nullptr);
    countEmitters();
    const int stayRoom = sStay + 1;
    writef(STDERR_FILENO, "[census] frame %u (%s): stage %s, Link in room %d\n", frames, why, sStage, sStay);
    // Link's room first (with the emitters), then the others that have actors
    for (int pass = 0; pass < 2; pass++) {
        for (int r = 0; r < kRooms; r++) {
            const bool stay = r == stayRoom || (stayRoom < 0 || stayRoom >= kRooms ? r == 0 : false);
            if (pass == 0 ? !stay : (stay || sRoomTotal[r] == 0)) continue;
            size_t len = 0;
            put(&len, "[census] stage %s room %d: ", sStage, r - 1);
            putActors(&len, r);
            if (pass == 0) putEmitters(&len);
            put(&len, "\n");
            writef(STDERR_FILENO, "%s", sLine);
        }
    }
}

} // namespace

const char* censusProcName(int name) {
    if (!sNamesReady) {
        for (const ProcName& p : kProcNames)
            if (p.id >= 0 && p.id < fpcNm_MAX_NUM_e) sNames[p.id] = p.name;
        sNamesReady = true;
    }
    if (name < 0 || name >= fpcNm_MAX_NUM_e || sNames[name] == nullptr) return "?";
    return sNames[name];
}

bool censusOn() {
    if (sOn < 0) {
        const char* v = getenv("COS_CENSUS");
        sOn = v != nullptr && (strcmp(v, "1") == 0 || strcmp(v, "on") == 0);
    }
    return sOn != 0;
}

void censusFrame(unsigned int frames) {
    if (!censusOn() || dComIfGp_getPlayer(0) == nullptr) return;
    const char* stage = dComIfGp_getStartStageName();
    const int stay = dComIfGp_roomControl_getStayNo();
    const uint64_t now = nowNs();
    const char* why = nullptr;
    if (strncmp(stage, sStage, sizeof(sStage) - 1) != 0) {
        why = "stage change";
    } else if (stay != sStay) {
        why = "room change";
    } else if (now - sLastNs >= kPeriodNs) {
        why = "every 5 s";
    }
    if (why == nullptr) return;
    snprintf(sStage, sizeof(sStage), "%s", stage);
    sStay = stay;
    sLastNs = now;
    report(why, frames);
}

} // namespace pc
