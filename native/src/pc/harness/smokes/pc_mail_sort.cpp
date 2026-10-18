// COS_SMOKE=mail-sort (issue #19): Koboli's letter sorting game in the Rito post office (d_a_npc_bmsw,
//   --stage Atorizk:0:0). Its three letters share one material whose image (one of six, one per box)
//   a texture pattern sets per letter just before the letter's model is entered. J3DDrawBuffer::
//   entryMatSort merges packets that isSame finds alike within a slot picked by a hash of the image's
//   address; the GameCube's addresses keep the six images in different slots, the host's put them in
//   two, so a letter was drawn with another letter's image (the one thrown, or the next one waiting).
// The game starts without the talk (pc_mail_sort_start) and a letter is thrown every kThrowEvery
// frames (pc_mail_sort_throw); every merge entryMatSort makes is checked: the packet joined must
// have the textures the list it joins was built with (J3DMatPacket::isSameTexture).
// Exit 0 after kThrows throws with no merge of different textures, else 1.
#include "pc_internal.h"

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "pc/game_hooks.h"

#include <cstring>
#include <unistd.h>

namespace {

constexpr int kThrows = 20;
constexpr int kThrowEvery = 25;
constexpr unsigned int kMaxFrames = 6000;

enum State { kOff, kOn, kDone };
State sState = kOff;
bool sStarted = false;
int sWaitFrames = 0;
int sThrows = 0;
int sMerges = 0;
int sBadMerges = 0;

bool active() {
    if (sState == kOff) {
        sState = pc::gConfig.smoke != nullptr && strcmp(pc::gConfig.smoke, "mail-sort") == 0 ? kOn : kDone;
    }
    return sState == kOn;
}

} // namespace

bool pc_mail_sort_start() {
    if (!active() || sStarted) {
        return false;
    }
    sStarted = true;
    return true;
}

bool pc_mail_sort_throw() {
    if (!active() || ++sWaitFrames < kThrowEvery) {
        return false;
    }
    sWaitFrames = 0;
    sThrows++;
    return true;
}

void pc_mail_sort_merged(J3DMatPacket* into, J3DMatPacket* joined) {
    if (!active()) {
        return;
    }
    sMerges++;
    if (!into->isSameTexture(joined->getMaterial()->getTevBlock(), j3dSys.getTexture())) {
        sBadMerges++;
    }
}

namespace pc {

void mailSortFrame(unsigned int frames) {
    if (!active() || (sThrows < kThrows && frames < kMaxFrames)) {
        return;
    }
    sState = kDone;
    const bool ok = sThrows >= kThrows && sBadMerges == 0;
    writef(STDERR_FILENO,
           "[cos] mail-sort: frame %u: %d letters thrown (at least %d), %d packet merges, %d of them with "
           "other textures: %s\n",
           frames, sThrows, kThrows, sMerges, sBadMerges, ok ? "PASS" : "FAIL");
    pc_exit(ok ? PC_EXIT_REACHED : PC_EXIT_CHECK_FAILED);
}

} // namespace pc
