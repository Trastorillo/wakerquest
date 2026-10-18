// COS_SMOKE=button-remap (issue #15, docs/BUTTON_REMAP_DESIGN.md): the per-action button remap
// (pc_button_remap.cpp) reaches what the game reads. Mac and Linux only: SDL virtual gamepads press
// physical buttons and axes before Aurora's mapping (COS_INPUT and PADSetVirtualStatus inject
// GameCube buttons after it, so they cannot test it). Once the play scene runs (e.g.
// --stage Asoko:0:0:2):
//   0. a virtual Pro Controller (vendor 0x057E, product 0x2009) is attached and must take port 0 (the
//      only port the game reads); a virtual GameCube adapter (product 0x0337) is attached next and
//      its Aurora button and axis mappings copied, before any remap;
//   then each check presses, waits kHoldFrames, reads, releases and waits kHoldFrames, and logs
//   "[cos] button-remap: <name>: ok|FAIL":
//   1. default: East (the A label) gives the game's B, South (B label) its A (the Mac's positional default,
//      defaultMapFor(false); the Switch's is by label and is checked by hand on the console);
//   2. swap: A given the A label (pc_button_remap_assign) swaps with B: East gives A, South B, and
//      the settings file has COS_PAD_A=a and COS_PAD_B=b;
//   3. trigger as button: Start given ZL (L takes Plus): the left trigger axis pressed fully gives
//      Start, and the analog L stays below 10% (the axis no longer feeds L);
//   4. button as L: the Plus button gives L with the analog L above 60% (a full digital press);
//   5. free input: Z given the free L shoulder: L gives Z, R (Z's old input, now free) gives nothing;
//   6. late controller: a second virtual Pro Controller attached now gets the remap: its East gives
//      A on its port (Aurora's PADStatus for that port, the game reads port 0 only);
//   7. gc adapter untouched: the GameCube adapter's mappings still equal the copy of step 0;
//   8. restore: pc_button_remap_restore_defaults leaves no COS_PAD_ line in the file; East gives B.
// Started with an invalid set (e.g. --env COS_PAD_A=b --env COS_PAD_B=b) the remap applies the
// default map with one log line: the smoke runs check 1 only (the environment locks the keys, so
// the later checks cannot assign) and exits after it.
// Exit 0 when every check is ok, else 1.
#include "pc_internal.h"

#include "features/pc_button_remap_map.h"
#include "pc/pc_button_remap.h"
#include "pc/pc_settings.h"

#include "JSystem/JUtility/JUTGamePad.h"
#include "d/d_com_inf_game.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_controller_pad.h"

#include <dolphin/pad.h>

#if !defined(__SWITCH__)
#include <SDL3/SDL.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace pc {

namespace {

enum State { kOff, kWait, kAttach, kRun, kDone };
State sState = kOff;

#if !defined(__SWITCH__)

namespace remap = pc::remap;

constexpr unsigned int kHoldFrames = 3;
constexpr unsigned int kAttachFrames = 120; // a virtual pad must reach Aurora (and get a port) by then
constexpr uint16_t kMapped = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_TRIGGER_Z |
                             PAD_TRIGGER_R | PAD_TRIGGER_L | PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y |
                             PAD_BUTTON_START;

// A virtual gamepad: SDL's joystick (whose buttons and axes the smoke sets) and the joystick
// button/axis behind each gamepad button/axis (SDL_GetGamepadBindings).
struct VPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joy = nullptr;
    SDL_Gamepad* pad = nullptr;
    int button[SDL_GAMEPAD_BUTTON_COUNT];
    int axis[SDL_GAMEPAD_AXIS_COUNT];
};
VPad sPro, sPro2, sGc;
std::vector<PADButtonMapping> sGcButtons;
std::vector<PADAxisMapping> sGcAxes;

bool attach(VPad& v, uint16_t product, const char* name) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x057E;
    desc.product_id = product;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (Uint32)((1u << SDL_GAMEPAD_BUTTON_COUNT) - 1);
    desc.axis_mask = (Uint32)((1u << SDL_GAMEPAD_AXIS_COUNT) - 1);
    desc.name = name;
    v.id = SDL_AttachVirtualJoystick(&desc);
    v.joy = v.id != 0 ? SDL_OpenJoystick(v.id) : nullptr;
    v.pad = v.id != 0 ? SDL_OpenGamepad(v.id) : nullptr;
    if (v.joy == nullptr || v.pad == nullptr) {
        writef(STDERR_FILENO, "[cos] button-remap: cannot attach the virtual %s: %s\n", name, SDL_GetError());
        return false;
    }
    for (int& b : v.button) {
        b = -1;
    }
    for (int& a : v.axis) {
        a = -1;
    }
    int count = 0;
    SDL_GamepadBinding** binds = SDL_GetGamepadBindings(v.pad, &count);
    for (int i = 0; binds != nullptr && i < count; i++) {
        const SDL_GamepadBinding* b = binds[i];
        if (b->input_type == SDL_GAMEPAD_BINDTYPE_BUTTON && b->output_type == SDL_GAMEPAD_BINDTYPE_BUTTON) {
            v.button[b->output.button] = b->input.button;
        } else if (b->input_type == SDL_GAMEPAD_BINDTYPE_AXIS && b->output_type == SDL_GAMEPAD_BINDTYPE_AXIS) {
            v.axis[b->output.axis.axis] = b->input.axis.axis;
        }
    }
    SDL_free(binds);
    return true;
}

// The Aurora port of a virtual pad (SDL's player index), once Aurora has the controller: -1 before.
int portOf(const VPad& v) {
    const int port = SDL_GetJoystickPlayerIndexForID(v.id);
    if (port < 0 || port >= PAD_MAX_CONTROLLERS || PADGetIndexForPort((u32)port) < 0) {
        return -1;
    }
    u32 vid = 0, pid = 0;
    PADGetVidPid((u32)port, &vid, &pid);
    return vid == SDL_GetJoystickVendorForID(v.id) && pid == SDL_GetJoystickProductForID(v.id) ? port : -1;
}

// What the game sees on a port: port 0 through mDoCPd_Convert's interface (the 12 buttons it maps
// and the analog L, 0..1), the others through Aurora's PADStatus as JUTGamePad::read kept it.
uint16_t heldOn(int port) {
    if (port == 0) {
        const interface_of_controller_pad& pad = g_mDoCPd_cpadInfo[0];
        uint16_t m = 0;
        m |= pad.mButtonHold.up ? PAD_BUTTON_UP : 0;
        m |= pad.mButtonHold.down ? PAD_BUTTON_DOWN : 0;
        m |= pad.mButtonHold.left ? PAD_BUTTON_LEFT : 0;
        m |= pad.mButtonHold.right ? PAD_BUTTON_RIGHT : 0;
        m |= pad.mButtonHold.z ? PAD_TRIGGER_Z : 0;
        m |= pad.mButtonHold.r ? PAD_TRIGGER_R : 0;
        m |= pad.mButtonHold.l ? PAD_TRIGGER_L : 0;
        m |= pad.mButtonHold.a ? PAD_BUTTON_A : 0;
        m |= pad.mButtonHold.b ? PAD_BUTTON_B : 0;
        m |= pad.mButtonHold.x ? PAD_BUTTON_X : 0;
        m |= pad.mButtonHold.y ? PAD_BUTTON_Y : 0;
        m |= pad.mButtonHold.start ? PAD_BUTTON_START : 0;
        return m;
    }
    return JUTGamePad::getPortStatus((JUTGamePad::EPadPort)port).button & kMapped;
}

// The settings file's lines.
std::vector<std::string> fileLines() {
    std::vector<std::string> lines;
    FILE* f = fopen(pc_settings_path(), "r");
    if (f == nullptr) {
        return lines;
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), f) != nullptr) {
        std::string l = buf;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) {
            l.pop_back();
        }
        lines.push_back(l);
    }
    fclose(f);
    return lines;
}

bool fileHas(const char* line) {
    for (const std::string& l : fileLines()) {
        if (l == line) {
            return true;
        }
    }
    return false;
}

bool fileHasRemapLine() {
    for (const std::string& l : fileLines()) {
        if (l.rfind("COS_PAD_", 0) == 0) {
            return true;
        }
    }
    return false;
}

bool gcMappingsKept() {
    const int port = portOf(sGc);
    u32 nb = 0, na = 0;
    const PADButtonMapping* b = port >= 0 ? PADGetButtonMappings((u32)port, &nb) : nullptr;
    const PADAxisMapping* a = port >= 0 ? PADGetAxisMappings((u32)port, &na) : nullptr;
    return b != nullptr && a != nullptr && nb == sGcButtons.size() && na == sGcAxes.size() &&
           memcmp(b, sGcButtons.data(), nb * sizeof(PADButtonMapping)) == 0 &&
           memcmp(a, sGcAxes.data(), na * sizeof(PADAxisMapping)) == 0;
}

// One press of a check: a button (or an axis to `value`) of a virtual pad; what the game must hold
// then on that pad's port (exactly, among the 12 mapped buttons), and the analog L range (port 0).
struct Step {
    const char* check;        // the check this press belongs to
    void (*before)();         // run once before the press (an assignment), or nullptr
    VPad* pad;
    int button;               // SDL_GamepadButton, or -1
    int axis;                 // SDL_GamepadAxis, or -1
    uint16_t want;
    float minL, maxL;         // the analog L must lie in [minL, maxL]
    bool (*after)();          // extra condition checked with the read, or nullptr
    bool last;                // the check ends with this step
};

bool sCheckFailed = false; // a press or condition of the current check failed
bool sAllOk = true;

void failNote(const char* what) {
    writef(STDERR_FILENO, "[cos] button-remap: %s\n", what);
    sCheckFailed = true;
}

void assignOrFail(remap::Action a, remap::Input in) {
    int other = -2;
    if (pc_button_remap_assign((int)a, (int)in, &other) != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "assign(%s, %s) failed", remap::actionKey(a), remap::inputValue(in));
        failNote(msg);
        return;
    }
    writef(STDERR_FILENO, "[cos] button-remap: assign(%s, %s): swapped with %s\n", remap::actionKey(a),
           remap::inputValue(in), other >= 0 ? remap::actionKey((remap::Action)other) : "nothing");
}

void beforeSwap() {
    assignOrFail(remap::Action::A, remap::Input::A);
}
bool afterSwap() {
    const bool ok = fileHas("COS_PAD_A=a") && fileHas("COS_PAD_B=b");
    if (!ok) {
        failNote("the settings file lacks COS_PAD_A=a / COS_PAD_B=b");
    }
    return ok;
}
void beforeTrigger() {
    assignOrFail(remap::Action::Start, remap::Input::ZL);
}
void beforeFree() {
    assignOrFail(remap::Action::Z, remap::Input::L);
}
void beforeLate() {
    if (!attach(sPro2, 0x2009, "button-remap Pro Controller 2")) {
        failNote("the second Pro Controller was not attached");
    }
}
bool afterGc() {
    const bool ok = gcMappingsKept();
    if (!ok) {
        failNote("the GameCube adapter's mappings changed (or it lost its port)");
    }
    return ok;
}
void beforeRestore() {
    pc_button_remap_restore_defaults();
}
bool afterRestore() {
    const bool ok = !fileHasRemapLine();
    if (!ok) {
        failNote("a COS_PAD_ line is still in the settings file");
    }
    return ok;
}

constexpr float kAnyL0 = -1.0f, kAnyL1 = 2.0f;
const Step kSteps[] = {
    {"default", nullptr, &sPro, SDL_GAMEPAD_BUTTON_EAST, -1, PAD_BUTTON_B, kAnyL0, kAnyL1, nullptr, false},
    {"default", nullptr, &sPro, SDL_GAMEPAD_BUTTON_SOUTH, -1, PAD_BUTTON_A, kAnyL0, kAnyL1, nullptr, true},
    {"swap", beforeSwap, &sPro, SDL_GAMEPAD_BUTTON_EAST, -1, PAD_BUTTON_A, kAnyL0, kAnyL1, afterSwap, false},
    {"swap", nullptr, &sPro, SDL_GAMEPAD_BUTTON_SOUTH, -1, PAD_BUTTON_B, kAnyL0, kAnyL1, nullptr, true},
    {"trigger as button", beforeTrigger, &sPro, -1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, PAD_BUTTON_START, kAnyL0, 0.1f,
     nullptr, true},
    {"button as L", nullptr, &sPro, SDL_GAMEPAD_BUTTON_START, -1, PAD_TRIGGER_L, 0.6f, kAnyL1, nullptr, true},
    {"free input", beforeFree, &sPro, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, -1, PAD_TRIGGER_Z, kAnyL0, kAnyL1, nullptr,
     false},
    {"free input", nullptr, &sPro, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1, 0, kAnyL0, kAnyL1, nullptr, true},
    {"late controller", beforeLate, &sPro2, SDL_GAMEPAD_BUTTON_EAST, -1, PAD_BUTTON_A, kAnyL0, kAnyL1, nullptr, true},
    {"gc adapter untouched", nullptr, nullptr, -1, -1, 0, kAnyL0, kAnyL1, afterGc, true},
    {"restore", beforeRestore, &sPro, SDL_GAMEPAD_BUTTON_EAST, -1, PAD_BUTTON_B, kAnyL0, kAnyL1, afterRestore, true},
};
constexpr size_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

// Per step: kBefore (run `before`), kPort (wait for the pad's port), kPress, kRead (after
// kHoldFrames), kRelease (kHoldFrames more), then the next step.
enum Phase { kBefore, kPort, kPress, kRead, kRelease };
size_t sStep = 0;
Phase sPhase = kBefore;
unsigned int sSince = 0;
unsigned int sAttachSince = 0;
bool sInvalidStart = false;

void setInput(const Step& s, bool down) {
    if (s.pad == nullptr) {
        return;
    }
    if (s.button >= 0) {
        SDL_SetJoystickVirtualButton(s.pad->joy, s.pad->button[s.button], down);
    } else if (s.axis >= 0) {
        SDL_SetJoystickVirtualAxis(s.pad->joy, s.pad->axis[s.axis], down ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    }
}

void finish(unsigned int frames, bool ok) {
    sState = kDone;
    writef(STDERR_FILENO, "[cos] button-remap: frame %u: %s\n", frames, ok ? "PASS" : "FAIL");
    pc_exit(ok ? PC_EXIT_REACHED : PC_EXIT_CHECK_FAILED);
}

// The twelve settings as the remap reads them: a valid set or not.
bool settingsValid() {
    remap::Map m = remap::defaultMap();
    for (size_t i = 0; i < m.size(); i++) {
        if (!remap::parseInput(pc_settings_get(remap::actionKey((remap::Action)i)), &m[i])) {
            return false;
        }
    }
    return remap::isValid(m);
}

void runSteps(unsigned int frames) {
    const Step& s = kSteps[sStep];
    switch (sPhase) {
    case kBefore:
        if (s.before != nullptr) {
            s.before();
        }
        sPhase = kPort;
        sSince = 0;
        return;
    case kPort:
        // The pad must be on a port, and the remap must have seen it (one more frame after that).
        if (s.pad != nullptr && portOf(*s.pad) < 0) {
            if (++sSince >= kAttachFrames) {
                failNote("the virtual pad got no Aurora port");
                sPhase = kRead; // reads nothing and fails
                sSince = kHoldFrames;
            }
            return;
        }
        sPhase = kPress;
        return;
    case kPress:
        setInput(s, true);
        sPhase = kRead;
        sSince = 0;
        return;
    case kRead: {
        if (++sSince < kHoldFrames) {
            return;
        }
        bool ok = true;
        if (s.pad != nullptr) {
            const int port = portOf(*s.pad);
            const uint16_t held = port >= 0 ? heldOn(port) : 0xFFFF;
            const float l = port == 0 ? g_mDoCPd_cpadInfo[0].mTriggerLeft : 0.0f;
            writef(STDERR_FILENO, "[cos] button-remap: %s: port %d, held 0x%04x (want 0x%04x), analog L %.2f\n",
                   s.check, port, (unsigned)held, (unsigned)s.want, l);
            if (held != s.want || l < s.minL || l > s.maxL) {
                ok = false;
            }
        }
        if (s.after != nullptr && !s.after()) {
            ok = false;
        }
        if (!ok) {
            sCheckFailed = true;
        }
        setInput(s, false);
        sPhase = kRelease;
        sSince = 0;
        return;
    }
    case kRelease:
        if (++sSince < kHoldFrames) {
            return;
        }
        if (s.last) {
            writef(STDERR_FILENO, "[cos] button-remap: %s: %s\n", s.check, sCheckFailed ? "FAIL" : "ok");
            sAllOk = sAllOk && !sCheckFailed;
            sCheckFailed = false;
            if (sInvalidStart) {
                writef(STDERR_FILENO,
                       "[cos] button-remap: started with an invalid set (the default map applies); "
                       "the later checks need assignable keys, skipped\n");
                finish(frames, sAllOk);
                return;
            }
        }
        sStep++;
        sPhase = kBefore;
        if (sStep >= kStepCount) {
            finish(frames, sAllOk);
        }
        return;
    }
}

#endif // !__SWITCH__

void* isPlayScene(void* proc, void*) {
    return fpcM_GetName(proc) == fpcNm_PLAY_SCENE_e ? proc : nullptr;
}

} // namespace

void buttonRemapFrame(unsigned int frames) {
    if (sState == kOff) {
        sState = gConfig.smoke != nullptr && strcmp(gConfig.smoke, "button-remap") == 0 ? kWait : kDone;
#if defined(__SWITCH__)
        if (sState == kWait) {
            writef(STDERR_FILENO, "[cos] button-remap: Mac and Linux only (SDL virtual gamepads)\n");
            pc_exit(PC_EXIT_CHECK_FAILED);
        }
#endif
    }
#if !defined(__SWITCH__)
    if (sState == kDone || dComIfGp_getPlayer(0) == nullptr || fpcM_Search(isPlayScene, nullptr) == nullptr) {
        return;
    }
    switch (sState) {
    case kOff:
    case kDone:
        return;
    case kWait:
        // Joystick input counts while the window has no focus (runs are not in front).
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        sInvalidStart = !settingsValid();
        if (!attach(sPro, 0x2009, "button-remap Pro Controller") ||
            !attach(sGc, 0x0337, "button-remap GameCube adapter")) {
            finish(frames, false);
            return;
        }
        sState = kAttach;
        sAttachSince = 0;
        return;
    case kAttach: {
        const int pro = portOf(sPro);
        const int gc = portOf(sGc);
        if (pro < 0 || gc < 0) {
            if (++sAttachSince >= kAttachFrames) {
                writef(STDERR_FILENO, "[cos] button-remap: the virtual pads got no Aurora port (Pro %d, GC %d)\n",
                       pro, gc);
                finish(frames, false);
            }
            return;
        }
        writef(STDERR_FILENO,
               "[cos] button-remap: frame %u: Pro Controller on port %d (SDL type %d, '%s'), GameCube adapter on "
               "port %d (SDL type %d, adapter %d)\n",
               frames, pro, (int)SDL_GetGamepadType(sPro.pad), PADGetName((u32)pro), gc,
               (int)SDL_GetGamepadType(sGc.pad), (int)PADIsGCAdapter((u32)gc));
        if (pro != 0) {
            writef(STDERR_FILENO, "[cos] button-remap: the virtual Pro Controller must be on port 0 (another "
                                  "controller is connected?)\n");
            finish(frames, false);
            return;
        }
        if (SDL_GetGamepadType(sPro.pad) != SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO) {
            writef(STDERR_FILENO, "[cos] button-remap: the virtual Pro Controller is not seen as one\n");
            finish(frames, false);
            return;
        }
        u32 nb = 0, na = 0;
        const PADButtonMapping* b = PADGetButtonMappings((u32)gc, &nb);
        const PADAxisMapping* a = PADGetAxisMappings((u32)gc, &na);
        if (b == nullptr || a == nullptr) {
            writef(STDERR_FILENO, "[cos] button-remap: no mappings for the GameCube adapter\n");
            finish(frames, false);
            return;
        }
        sGcButtons.assign(b, b + nb);
        sGcAxes.assign(a, a + na);
        sState = kRun;
        return;
    }
    case kRun:
        runSteps(frames);
        return;
    }
#else
    (void)frames;
    (void)isPlayScene;
#endif
}

} // namespace pc
