// Button remapping (issue #15, docs/BUTTON_REMAP_DESIGN.md; native/include/pc/pc_button_remap.h): the
// twelve COS_PAD_* settings, the map they make (pc_button_remap_map.h: validation and swap logic), and
// that map written into Aurora's per-controller mappings.
//
// The map reaches Aurora once per game frame (pc_button_remap_frame), when the settings changed or the
// controller on a port 0-3 changed (a pad plugged in, Joy-Cons attached): Aurora keeps one mapping per
// controller and gives a new one its type's default table, so a controller connected later gets the
// remap on the frame it appears. Each controller of a standard type (see isStandard) gets:
//   - the map is the default: Aurora's own default table back (PADRestoreDefaultMapping, buttons and
//     axes), so a player who never remaps plays exactly as before;
//   - otherwise one PADButtonMapping per action (the input's SDL gamepad button, or
//     PAD_NATIVE_TRIGGER_LEFT/RIGHT for ZL/ZR: Aurora patch 0022 reads the trigger axis as a button).
//     L and R are analog too: on ZL/ZR their button entry is empty and PAD_AXIS_TRIGGER_L/R reads that
//     trigger axis (Aurora's emulateTriggers gives the digital bit past its activation zone, the light
//     press keeps working); on a button, the axis mapping reads that button, which Aurora turns into
//     a full analog value with the digital bit. Both trigger axis mappings are rewritten every time,
//     so a trigger that no longer feeds L/R never reaches them.
// PADGetButtonMappings runs first: it loads Aurora's own mapping file for the controller, if any,
// which would otherwise replace ours on the first PADRead.
#include "pc/pc_button_remap.h"

#include "pc/pc_settings.h"
#include "pc_internal.h"
#include "features/pc_button_remap_map.h"

#include <dolphin/pad.h>

#include <SDL3/SDL_gamepad.h>

#include <cstring>
#include <string>
#include <unistd.h>

namespace {

namespace remap = pc::remap;
using remap::Action;
using remap::Input;

constexpr size_t kActions = (size_t)Action::Count;
constexpr size_t kInputs = (size_t)Input::Count;

// The menu's values, named as printed on a Switch controller (Task 3 shows them on the remap rows).
const PcSettingChoice kChoices[kInputs] = {
    {"a", "A", nullptr},
    {"b", "B", nullptr},
    {"x", "X", nullptr},
    {"y", "Y", nullptr},
    {"l", "L", nullptr},
    {"r", "R", nullptr},
    {"zl", "ZL", nullptr},
    {"zr", "ZR", nullptr},
    {"plus", "+", nullptr},
    {"up", "Cruceta arriba", "D-pad up"},
    {"down", "Cruceta abajo", "D-pad down"},
    {"left", "Cruceta izquierda", "D-pad left"},
    {"right", "Cruceta derecha", "D-pad right"},
    {"lstick", "L3 (stick izquierdo)", "L3 (left stick)"},
    {"rstick", "R3 (stick derecho)", "R3 (right stick)"},
};

// The GameCube actions as the rows name them, and the PADStatus bit each one sets.
struct ActionInfo {
    const char* label;
    const char* labelEn;
    PADButton padButton;
};
const ActionInfo kActionInfo[kActions] = {
    {"A", "A", PAD_BUTTON_A},
    {"B", "B", PAD_BUTTON_B},
    {"X", "X", PAD_BUTTON_X},
    {"Y", "Y", PAD_BUTTON_Y},
    {"Z", "Z", PAD_TRIGGER_Z},
    {"L", "L", PAD_TRIGGER_L},
    {"R", "R", PAD_TRIGGER_R},
    {"START", "START", PAD_BUTTON_START},
    {"Cruceta arriba", "D-pad up", PAD_BUTTON_UP},
    {"Cruceta abajo", "D-pad down", PAD_BUTTON_DOWN},
    {"Cruceta izquierda", "D-pad left", PAD_BUTTON_LEFT},
    {"Cruceta derecha", "D-pad right", PAD_BUTTON_RIGHT},
};

// The SDL gamepad button (or Aurora's trigger-as-button value) behind each input. The face buttons
// are named by their printed letter: SDL 3 on the Mac and Linux reports them by position (a Switch
// controller's A is east), Aurora's SDL shim on the Switch by label (the Switch's A is south: it maps
// HidNpadButton_A to SDL_GAMEPAD_BUTTON_SOUTH, switch/aurora/sdl3_shim).
u32 nativeButton(Input in) {
    switch (in) {
#if defined(__SWITCH__)
    case Input::A: return SDL_GAMEPAD_BUTTON_SOUTH;
    case Input::B: return SDL_GAMEPAD_BUTTON_EAST;
    case Input::X: return SDL_GAMEPAD_BUTTON_WEST;
    case Input::Y: return SDL_GAMEPAD_BUTTON_NORTH;
#else
    case Input::A: return SDL_GAMEPAD_BUTTON_EAST;
    case Input::B: return SDL_GAMEPAD_BUTTON_SOUTH;
    case Input::X: return SDL_GAMEPAD_BUTTON_NORTH;
    case Input::Y: return SDL_GAMEPAD_BUTTON_WEST;
#endif
    case Input::L: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case Input::R: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case Input::ZL: return PAD_NATIVE_TRIGGER_LEFT;
    case Input::ZR: return PAD_NATIVE_TRIGGER_RIGHT;
    case Input::Plus: return SDL_GAMEPAD_BUTTON_START;
    case Input::Up: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case Input::Down: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case Input::Left: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case Input::Right: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    case Input::LStick: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case Input::RStick: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    default: return PAD_NATIVE_BUTTON_INVALID;
    }
}

struct Remap {
    bool initialized = false;
    bool dirty = true;
    bool appliedOnce = false;
    bool applyPending = false;            // the map in effect changed since the last apply
    remap::Map map = remap::defaultMap(); // in effect
    std::string invalidLogged;            // the invalid set logged last ("": none)
    // the controller on each port when the map was last applied
    s32 index[PAD_MAX_CONTROLLERS];
    SDL_JoystickID instance[PAD_MAX_CONTROLLERS];
    u32 vid[PAD_MAX_CONTROLLERS];
    u32 pid[PAD_MAX_CONTROLLERS];
    PcSettingDesc descs[kActions];
    std::string helps[kActions], helpsEn[kActions];
} r;

void markDirty(const char*, const char*, void*) {
    r.dirty = true;
}

// The twelve settings as a map; false (and the raw values in `what`) for an invalid set.
bool readSettings(remap::Map& out, std::string& what) {
    remap::Map m = remap::defaultMap();
    bool parsed = true;
    what.clear();
    for (size_t i = 0; i < kActions; i++) {
        const char* key = remap::actionKey((Action)i);
        const char* value = pc_settings_get(key);
        if (!remap::parseInput(value, &m[i])) {
            parsed = false;
        }
        what += std::string(what.empty() ? "" : " ") + key + "=" + value;
    }
    if (!parsed || !remap::isValid(m)) {
        return false;
    }
    out = m;
    return true;
}

// The controller types whose Aurora default is the standard (Pro Controller-shaped) table. The
// GameCube adapter, the NSO GameCube controller (Pro type, product 0x2073), a single Joy-Con and
// unknown types keep Aurora's own table: theirs already match the GameCube's buttons.
bool isStandard(u32 port, SDL_Gamepad* gp) {
    if (gp == nullptr || PADIsGCAdapter(port)) {
        return false;
    }
    u32 vid = 0, pid = 0;
    PADGetVidPid(port, &vid, &pid);
    (void)vid;
    switch (SDL_GetGamepadType(gp)) {
    case SDL_GAMEPAD_TYPE_STANDARD:
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return true;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        return pid != 0x2073;
    default:
        return false;
    }
}

bool isDefault(const remap::Map& m) {
    return m == remap::defaultMap();
}

// The trigger axis mapping of L or R: the trigger axis of ZL/ZR, or the button it now sits on.
PADAxisMapping triggerAxis(PADAxis axis, Input in) {
    PADAxisMapping a{};
    a.padAxis = axis;
    if (in == Input::ZL || in == Input::ZR) {
        a.nativeAxis = {in == Input::ZL ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                        AXIS_SIGN_POSITIVE};
        a.nativeButton = SDL_GAMEPAD_BUTTON_INVALID;
    } else {
        a.nativeAxis = {-1, AXIS_SIGN_POSITIVE};
        a.nativeButton = (s32)nativeButton(in);
    }
    return a;
}

void applyPort(u32 port) {
    const s32 index = PADGetIndexForPort(port);
    SDL_Gamepad* gp = index >= 0 ? PADGetSDLGamepadForIndex((u32)index) : nullptr;
    if (!isStandard(port, gp)) {
        return;
    }
    u32 count = 0;
    PADGetButtonMappings(port, &count); // loads Aurora's own mapping file first (see the top)
    if (isDefault(r.map)) {
        PADRestoreDefaultMapping(port);
        return;
    }
    // One PADSetButtonMapping per action: Aurora defines PADSetAllButtonMappings with a const array, so
    // its C++ definition does not match the extern "C" declaration in pad.h and the symbol is missing.
    for (size_t i = 0; i < kActions; i++) {
        const Action a = (Action)i;
        const Input in = r.map[i];
        const bool analog = (a == Action::L || a == Action::R) && (in == Input::ZL || in == Input::ZR);
        PADSetButtonMapping(port, {analog ? PAD_NATIVE_BUTTON_INVALID : nativeButton(in), kActionInfo[i].padButton});
    }
    PADSetAxisMapping(port, triggerAxis(PAD_AXIS_TRIGGER_L, r.map[(size_t)Action::L]));
    PADSetAxisMapping(port, triggerAxis(PAD_AXIS_TRIGGER_R, r.map[(size_t)Action::R]));
}

// Saves `m` (the twelve actions): an action on its default input loses its line, the others are set.
// Nothing is written and -1 returned when a key that must change is locked by the environment (a
// half-saved swap would be an invalid set).
int save(const remap::Map& m) {
    const remap::Map def = remap::defaultMap();
    bool change[kActions] = {};
    for (size_t i = 0; i < kActions; i++) {
        const char* key = remap::actionKey((Action)i);
        const bool same = strcmp(pc_settings_get(key), remap::inputValue(m[i])) == 0;
        // An action on its default input whose value is still stored (a line equal to the default)
        // is cleared too, unless the environment holds it.
        change[i] = !same || (m[i] == def[i] && !pc_settings_locked(key));
        if (!same && pc_settings_locked(key)) {
            pc::writef(STDERR_FILENO, "[cos] button-remap: %s is set by the environment, not changed\n", key);
            return -1;
        }
    }
    for (size_t i = 0; i < kActions; i++) {
        if (!change[i]) {
            continue;
        }
        const char* key = remap::actionKey((Action)i);
        if ((m[i] == def[i] ? pc_settings_clear(key) : pc_settings_set(key, remap::inputValue(m[i]))) != 0) {
            return -1;
        }
    }
    return 0;
}

// Re-reads the settings after a change (dirty): the map in effect, the default one for an invalid
// set (logged once per different invalid set). True when the map in effect changed.
bool refresh() {
    if (!r.dirty) {
        return false;
    }
    r.dirty = false;
    remap::Map m;
    std::string what;
    if (readSettings(m, what)) {
        r.invalidLogged.clear();
    } else {
        m = remap::defaultMap();
        if (what != r.invalidLogged) {
            pc::writef(STDERR_FILENO,
                       "[cos] button-remap: invalid set (an unknown value or an input used twice), the default "
                       "map applies: %s\n",
                       what.c_str());
            r.invalidLogged = what;
        }
    }
    if (m == r.map) {
        return false;
    }
    r.map = m;
    r.applyPending = true;
    return true;
}

} // namespace

extern "C" {

void pc_button_remap_init(void) {
    if (r.initialized) {
        return;
    }
    r.initialized = true;
    for (u32 p = 0; p < PAD_MAX_CONTROLLERS; p++) {
        r.index[p] = -2; // differs from any port's value: the first frame applies
        r.instance[p] = 0;
        r.vid[p] = r.pid[p] = 0;
    }
    const remap::Map def = remap::defaultMap();
    for (size_t i = 0; i < kActions; i++) {
        const ActionInfo& info = kActionInfo[i];
        r.helps[i] = std::string("El botón del mando que hace de ") + info.label + " de GameCube.";
        r.helpsEn[i] = std::string("The controller button that acts as the GameCube's ") + info.labelEn + ".";
        PcSettingDesc& d = r.descs[i];
        d = PcSettingDesc{};
        d.key = remap::actionKey((Action)i);
        d.label = info.label;
        d.help = r.helps[i].c_str();
        d.tab = PC_SETTING_TAB_CONTROLS;
        d.flags = PC_SETTING_REMAP;
        d.choices = kChoices;
        d.choiceCount = (int)kInputs;
        d.defaultValue = remap::inputValue(def[i]);
        d.apply = nullptr;
        d.user = nullptr;
        d.order = 10 * (int)(i + 1);
        d.labelEn = info.labelEn;
        d.helpEn = r.helpsEn[i].c_str();
        pc_settings_register(&d);
        pc_settings_subscribe(d.key, markDirty, nullptr);
    }
    r.dirty = true;
}

void pc_button_remap_frame(void) {
    if (!r.initialized) {
        return;
    }
    refresh();
    bool apply = !r.appliedOnce || r.applyPending;
    for (u32 p = 0; p < PAD_MAX_CONTROLLERS; p++) {
        const s32 index = PADGetIndexForPort(p);
        SDL_Gamepad* gp = index >= 0 ? PADGetSDLGamepadForIndex((u32)index) : nullptr;
        const SDL_JoystickID instance = gp != nullptr ? SDL_GetJoystickID(SDL_GetGamepadJoystick(gp)) : 0;
        u32 vid = 0, pid = 0;
        PADGetVidPid(p, &vid, &pid);
        if (index != r.index[p] || instance != r.instance[p] || vid != r.vid[p] || pid != r.pid[p]) {
            r.index[p] = index;
            r.instance[p] = instance;
            r.vid[p] = vid;
            r.pid[p] = pid;
            apply = true;
        }
    }
    if (!apply) {
        return;
    }
    r.appliedOnce = true;
    r.applyPending = false;
    for (u32 p = 0; p < PAD_MAX_CONTROLLERS; p++) {
        applyPort(p);
    }
}

int pc_button_remap_assign(int action, int input, int* swappedAction) {
    if (swappedAction != nullptr) {
        *swappedAction = -1;
    }
    if (action < 0 || action >= (int)kActions || input < 0 || input >= (int)kInputs) {
        return -1;
    }
    // On the map in effect, with a change of this frame read already (two assignments in one frame);
    // the next pc_button_remap_frame applies it.
    refresh();
    const remap::AssignResult res = remap::assign(r.map, (Action)action, (Input)input);
    if (save(res.map) != 0) {
        return -1;
    }
    if (swappedAction != nullptr && res.swapped) {
        *swappedAction = (int)res.other;
    }
    pc::writef(STDERR_FILENO, "[cos] button-remap: %s = %s%s%s\n", remap::actionKey((Action)action),
               remap::inputValue((Input)input), res.swapped ? ", swapped with " : "",
               res.swapped ? remap::actionKey(res.other) : "");
    return 0;
}

void pc_button_remap_restore_defaults(void) {
    for (size_t i = 0; i < kActions; i++) {
        pc_settings_clear(remap::actionKey((Action)i));
    }
    pc::writef(STDERR_FILENO, "[cos] button-remap: defaults restored\n");
}

int pc_button_remap_current(int action) {
    if (action < 0 || action >= (int)kActions) {
        return -1;
    }
    refresh();
    return (int)r.map[(size_t)action];
}

} // extern "C"
