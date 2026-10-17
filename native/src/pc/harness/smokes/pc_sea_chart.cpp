// COS_SMOKE=sea-chart (issue #14): the sea chart's island check points (/res/FmapDat/FmapDat.bin, read by
// dMap_c::setFmapChkDtPrm) give each island's sector to the chart's zooms. Once the play scene runs:
//   1. the count is at least kMinPoints (the disc has one per island sector);
//   2. every point's sector is on the 7 x 7 grid (-3..3) and dMap_getCheckPointUseGrid finds that point
//      again from its sector, as setDispIslandPos does for the zooms;
//   3. some points lie in the western and northern sectors (negative coordinates).
// Before the fix the count was read in host order (0: no sector found an island, so the 1000 x 1000
// zoom stayed empty and the 200 x 200 one kept Forsaken Fortress) and the sectors were char, unsigned
// on ARM (no negative sector matched).
// Exit 0 when all three hold, else 1.
#include "pc_internal.h"

#include "d/d_com_inf_game.h"
#include "d/d_map.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"

#include <cstring>
#include <unistd.h>

namespace pc {

namespace {

constexpr int kMinPoints = 40;

enum State { kOff, kWait, kDone };
State sState = kOff;

void* isPlayScene(void* proc, void*) {
    return fpcM_GetName(proc) == fpcNm_PLAY_SCENE_e ? proc : nullptr;
}

} // namespace

void seaChartFrame(unsigned int frames) {
    if (sState == kOff) {
        sState = gConfig.smoke != nullptr && strcmp(gConfig.smoke, "sea-chart") == 0 ? kWait : kDone;
    }
    if (sState != kWait || dComIfGp_getPlayer(0) == nullptr || fpcM_Search(isPlayScene, nullptr) == nullptr) {
        return;
    }
    sState = kDone;

    const int count = dComIfGp_getFmapData() != nullptr ? (int)(u32)((dMap_FmapChkPnt*)dComIfGp_getFmapData())->field_0x0 : -1;
    int found = 0;
    int west = 0;
    int north = 0;
    for (int i = 0; i < count && i < 49; i++) {
        s8 gx;
        s8 gy;
        s16 x;
        s16 y;
        dMap_getFmapChkPntPrm(i, &gx, &gy, &x, &y, NULL);
        const int again = dMap_getCheckPointUseGrid(gx, gy);
        if (gx >= -3 && gx <= 3 && gy >= -3 && gy <= 3 && again == i) {
            found++;
        } else {
            writef(STDERR_FILENO, "[cos] sea-chart: point %d: sector %d,%d found again as %d\n", i, gx, gy, again);
        }
        west += gx < 0;
        north += gy < 0;
    }
    const bool ok = count >= kMinPoints && found == count && west > 0 && north > 0;
    writef(STDERR_FILENO,
           "[cos] sea-chart: frame %u: %d check points (at least %d), %d found from their sector, %d western, "
           "%d northern: %s\n",
           frames, count, kMinPoints, found, west, north, ok ? "PASS" : "FAIL");
    pc_exit(ok ? PC_EXIT_REACHED : PC_EXIT_CHECK_FAILED);
}

} // namespace pc
