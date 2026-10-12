// COS_SMOKE=sun-glare (issue #7): the sun's lens flare and glare, which the game shows only where
// GXPeekZ reads the sky's depth (the far plane, 0xFFFFFF) at the sun's five check points
// (d_kankyo_rain.cpp dKyr_sun_move peeks them through dComIfGd_peekZ at the end of each frame and
// reads them in the next). Run with the sailing preset at 17:30 (COS_BOOT_TIME=17.5) and
// native/check/input/sun-glare.txt, which turns the camera until the low sun is near the top
// centre of the view and lets it settle. At kCheckFrame the sun packet must have its centre point
// and at least four of the five on the sky, a visibility near 1 and the flare drawn over the scene
// (mbDrawLenzInSky false). With GXPeekZ reading 0 (the deko3d renderer before the fix) every point
// reads occluded and the flare and glare never draw. Exit 0 when all held, else 1. A shot of it.
#include "pc_internal.h"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_kankyo_wether.h"

#include <cstring>
#include <unistd.h>

namespace pc {

namespace {

constexpr unsigned int kCheckFrame = 1100; // the script stops the camera at 983
bool sChecked = false;
bool sOn = false;

void shotSink(unsigned int frame, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h, void*) {
    saveFramePng(frame, rgb, w, h);
}

} // namespace

void sunGlareFrame(unsigned int frames) {
    if (!sChecked) {
        sChecked = true;
        sOn = gConfig.smoke != nullptr && strcmp(gConfig.smoke, "sun-glare") == 0;
    }
    if (!sOn || frames < kCheckFrame) {
        return;
    }
    const dKankyo_sun_Packet* sun = dKy_getEnvlight().mpSunPacket;
    const dKankyo_sunlenz_Packet* lenz = dKy_getEnvlight().mpSunlenzPacket;
    if (sun == nullptr || lenz == nullptr) {
        writef(STDERR_FILENO, "[cos] sun-glare: frame %u: no sun packet (not in play at sea in daytime?)\nFAIL\n",
               frames);
        pc_exit(PC_EXIT_CHECK_FAILED);
    }
    int onSky = 0;
    for (int i = 0; i < 5; i++) {
        onSky += sun->mVizChkData[i] >= 0xFFFFFF;
    }
    const bool centre = sun->mVizChkData[0] >= 0xFFFFFF;
    writef(STDERR_FILENO,
           "[cos] sun-glare: frame %u: time %.1f, peeked depth %06x %06x %06x %06x %06x (%d of 5 on the sky), "
           "visibility %.2f, flare %s\n",
           frames, dKy_getEnvlight().mCurTime, (unsigned)sun->mVizChkData[0], (unsigned)sun->mVizChkData[1],
           (unsigned)sun->mVizChkData[2], (unsigned)sun->mVizChkData[3], (unsigned)sun->mVizChkData[4], onSky,
           sun->mVisibility, lenz->mbDrawLenzInSky ? "behind the scene (sun hidden)" : "over the scene");
    captureFrame(frames, shotSink, nullptr);
    const bool ok = centre && onSky >= 4 && sun->mVisibility > 0.9f && !lenz->mbDrawLenzInSky;
    writef(STDERR_FILENO, "[cos] sun-glare: %s\n", ok ? "PASS" : "FAIL");
    pc_exit(ok ? PC_EXIT_REACHED : PC_EXIT_CHECK_FAILED);
}

} // namespace pc
