// COS_SMOKE=menu-backdrop (issue #20): the pause menu (Start) opens over the frame the game copied into
// mDoGph_gInf_c's frame buffer texture (dDlst_MENU_CAPTURE_c draws it until the menu's cloth covers the
// screen). The game makes that copy every frame in the depth-of-field pass; with COS_DOF=0 the port
// left the copy out, so the menu opened over black (nothing copied since the boot) or over the last
// frame of a blur or monotone effect. Run with --env COS_DOF=0, e.g. --stage Asoko:0:0:2:
//   1. A through any event until none runs, then kSettleFrames;
//   2. Start pressed until the menu opens (dMenu_flag), within kOpenFrames;
//   3. kDrawFrames later, a shot: the top left of the picture (kArea*, fractions of the picture, still
//      outside the cloth) must show the scene, not black (mean of the channels at least kMinMean).
// Exit 0 when it does, else 1.
#include "pc_internal.h"

#include "d/d_com_inf_game.h"
#include "d/d_meter.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"

#include <dolphin/pad.h>

#include <cstring>
#include <unistd.h>

namespace pc {

namespace {

constexpr unsigned int kSettleFrames = 60;
constexpr unsigned int kOpenFrames = 60;
constexpr unsigned int kDrawFrames = 2;
// Below the hearts, left of where the cloth comes in from the bottom right in the first frames.
constexpr float kAreaX0 = 0.04f, kAreaX1 = 0.30f, kAreaY0 = 0.14f, kAreaY1 = 0.40f;
constexpr unsigned int kMinMean = 25; // 13 before the fix (black, the menu's shading over it); the Outset
                                      // house's hall: 41

enum State { kOff, kWait, kOpen, kDraw, kDone };
State sState = kOff;
unsigned int sSince = 0;
unsigned int sEventFrames = 0;

struct Mean {
    uint64_t sum = 0;
    uint64_t count = 0;
};

void* isPlayScene(void* proc, void*) {
    return fpcM_GetName(proc) == fpcNm_PLAY_SCENE_e ? proc : nullptr;
}

void meanSink(unsigned int frame, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h, void* user) {
    saveFramePng(frame, rgb, w, h);
    Mean* m = static_cast<Mean*>(user);
    for (uint32_t y = (uint32_t)(kAreaY0 * h); y < (uint32_t)(kAreaY1 * h); y++) {
        for (uint32_t x = (uint32_t)(kAreaX0 * w); x < (uint32_t)(kAreaX1 * w); x++) {
            const uint8_t* p = &rgb[(y * w + x) * 3];
            m->sum += p[0] + p[1] + p[2];
            m->count += 3;
        }
    }
}

void finish(unsigned int frames, bool ok) {
    setDrivenPad(true, 0, 0, 0);
    sState = kDone;
    writef(STDERR_FILENO, "[cos] menu-backdrop: frame %u: %s\n", frames, ok ? "PASS" : "FAIL");
    pc_exit(ok ? PC_EXIT_REACHED : PC_EXIT_CHECK_FAILED);
}

} // namespace

void menuBackdropFrame(unsigned int frames) {
    if (sState == kOff) {
        if (gConfig.smoke == nullptr || strcmp(gConfig.smoke, "menu-backdrop") != 0) {
            sState = kDone;
            return;
        }
        setDrivenPad(true, 0, 0, 0);
        sState = kWait;
    }
    if (sState == kDone || dComIfGp_getPlayer(0) == nullptr || fpcM_Search(isPlayScene, nullptr) == nullptr) {
        return;
    }
    ++sSince;
    switch (sState) {
    case kOff:
    case kDone:
        return;
    case kWait:
        if (dComIfGp_event_runCheck()) {
            sEventFrames++;
            setDrivenPad(true, sEventFrames % 20 < 2 ? PAD_BUTTON_A : 0, 0, 0);
            sSince = 0;
            return;
        }
        setDrivenPad(true, 0, 0, 0);
        if (sSince >= kSettleFrames) {
            writef(STDERR_FILENO, "[cos] menu-backdrop: frame %u: Start\n", frames);
            sState = kOpen;
            sSince = 0;
        }
        return;
    case kOpen:
        setDrivenPad(true, sSince % 20 < 2 ? PAD_BUTTON_START : 0, 0, 0);
        if (dMenu_flag() != 0) {
            setDrivenPad(true, 0, 0, 0);
            writef(STDERR_FILENO, "[cos] menu-backdrop: frame %u: the menu opened\n", frames);
            sState = kDraw;
            sSince = 0;
        } else if (sSince >= kOpenFrames) {
            writef(STDERR_FILENO, "[cos] menu-backdrop: Start did not open the menu\n");
            finish(frames, false);
        }
        return;
    case kDraw: {
        if (sSince < kDrawFrames) {
            return;
        }
        Mean m;
        if (!captureFrame(frames, meanSink, &m) || m.count == 0) {
            writef(STDERR_FILENO, "[cos] menu-backdrop: the frame could not be read back\n");
            finish(frames, false);
            return;
        }
        const unsigned int mean = (unsigned int)(m.sum / m.count);
        writef(STDERR_FILENO, "[cos] menu-backdrop: frame %u: backdrop area mean %u (at least %u)\n", frames,
               mean, kMinMean);
        finish(frames, mean >= kMinMean);
        return;
    }
    }
}

} // namespace pc
