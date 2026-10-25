// WakerQuest's settings panel in the headset, after GalaxyQuest's VR settings panel
// (platform/src/xr/vr_settings.cpp): a panel drawn by the headset layer on the CPU (ui_canvas.h) into
// a quad layer of its own, sharp whatever the game's resolution, in front of the player in either
// mode (the giant screen or the diorama). It opens with both triggers and the right grip (as the
// game's options menu did), and is worked with the laser pointer (either hand's trigger) or the
// controller. The game waits while it is open (pc_vr_panel_frame, the game thread).
//
// Its settings are of two kinds: xr.ini's (the headset layer's own, set at once through the Host),
// and a few of the game's (pc_settings: the language, the camera...), which the game thread mirrors
// every frame and applies (wq_xr_panel_* in wq_xr.h). The other game settings stay in the game's
// options menu, a button away ("Más opciones").
#include "wq_panel.h"

#include "ui_canvas.h"
#include "wq_xr.h"

#include <GLES3/gl3.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <jni.h>
#include <EGL/egl.h>
#include <openxr/openxr_platform.h>

namespace {

using ui::Canvas;
using ui::Color;
using ui::kFontLarge;
using ui::kFontSmall;
using ui::rgb;

void plog(const char* what, const char* detail = "") {
    fprintf(stderr, "[xr] panel: %s%s\n", what, detail);
    fflush(stderr);
}

// ---- The settings ------------------------------------------------------------------------------------

enum Source { kXr, kGame, kAction };
enum Kind { kSwitch, kStepper, kButton };

struct Option {
    const char* value;
    const char* text;
};

struct Item {
    Source src;
    const char* key;  // xr.ini's or pc_settings'; for kAction, what it does
    const char* label;
    const char* hint;
    Kind kind;
    const Option* options;  // the values to step through, or a number range:
    int optionCount;
    float min, max, step;
    const char* format;     // of the number ("%.1f m"); the decimal point shown as a comma
};

constexpr int kLabelMaxW = 640;  // the size label's chain (wq_xr.cpp: the window's line is longer)

#define OPTS(o) o, (int)(sizeof(o) / sizeof(o[0]))
const Option kMode[] = {{"pantalla", "Pantalla"}, {"maqueta", "Maqueta"}, {"ventana", "Ventana"}};
const Option kShape[] = {{"esfera", "Esfera"}, {"caja", "Caja"}};
const Option kRefresh[] = {{"72", "72 Hz"}, {"80", "80 Hz"}, {"90", "90 Hz"}, {"120", "120 Hz"}};
const Option kResolution[] = {{"540", "0,50"},   {"648", "0,60"},   {"756", "0,70"},   {"864", "0,80"},
                              {"918", "0,85"},   {"972", "0,90"},   {"1080", "1,00"},  {"1188", "1,10"},  {"1296", "1,20"},
                              {"1404", "1,30"},  {"1512", "1,40"},  {"1620", "1,50"},  {"1728", "1,60"},
                              {"1944", "1,80"},  {"2160", "2,00"}};
// the Rendimiento preset (wq_xr.cpp kPresetKeys): refresh, GPU, resolution, 3D, the bubble's world, the
// window's horizon, the characters' distance, set at once when one is chosen; personal = the values chosen by hand
const Option kPerfPreset[] = {{"ahorro", "Ahorro"}, {"equilibrado", "Equilibrado"}, {"maximo", "Máximo"},
                              {"personal", "Personal"}};
const Option kAnchor[] = {{"world", "Habitación"}, {"camera", "Cámara"}};
const Option kLanguage[] = {{"auto", "Auto"},       {"es", "Español"}, {"en", "English"},
                            {"fr", "Français"},     {"de", "Deutsch"}, {"it", "Italiano"}};

const Item kGeneral[] = {
    {kXr, "perf_preset", "Rendimiento", "Ahorro: menos calor; Máximo: más nitidez y 3D", kStepper, OPTS(kPerfPreset)},
    {kXr, "mode", "Modo", "Pantalla gigante, el mundo en una burbuja, o una ventana", kStepper, OPTS(kMode)},
    {kGame, "COS_VR_RESOLUTION", "Resolución", "La de este modo (y del 3D); 1,00 = 1080 líneas", kStepper,
     OPTS(kResolution)},
    {kXr, "refresh", "Frecuencia", "72 Hz: menos calor; 90 Hz: más fluido", kStepper, OPTS(kRefresh)},
    {kXr, "gpu_boost", "Más potencia de GPU", "Sube el reloj de la GPU; gasta más batería", kSwitch},
    // the giant screen: also moved with the left grip and scaled with both (wq_xr.cpp)
    {kXr, "distance", "Distancia de la pantalla", "O muévela con el grip izquierdo", kStepper, nullptr, 0, 1.0f, 20.0f,
     0.5f, "%.1f m"},
    {kXr, "width", "Tamaño de la pantalla", "O agárrala con los dos grips", kStepper, nullptr, 0, 1.0f, 20.0f, 0.5f,
     "%.1f m"},
    // the window (mode = ventana); its size and place with the grips, the rest in Efectos
    {kXr, "window_scale", "Escala de la ventana", "Más = el mundo más pequeño; 1 = tamaño real", kStepper, nullptr, 0,
     0.25f, 5.0f, 0.25f, "%.2f"},
    {kXr, "window_popout_on", "Salir de la ventana", "Lo cercano sale hacia ti", kSwitch},
    {kXr, "window_popout", "Cuánto sale", "Metros por delante de la ventana", kStepper, nullptr, 0, 0.1f, 2.0f, 0.1f,
     "%.1f m"},
};
const Item kStereo[] = {
    {kXr, "stereo", "3D en la pantalla", "Una imagen para cada ojo", kSwitch},
    {kXr, "stereo_strength", "Profundidad 3D", "Más = el fondo más lejos", kStepper, nullptr, 0, 0.004f, 0.10f,
     0.002f, "%.3f"},
    {kXr, "stereo_focus", "Salir de la pantalla", "Más = lo cercano sale hacia ti", kStepper, nullptr, 0, 50.0f,
     2500.0f, 100.0f, "%.0f"},
    {kXr, "cine_3d", "3D intenso en cinemáticas", "Más profundidad en las escenas", kSwitch},
    {kXr, "cine_screen", "Cinemáticas en pantalla gigante", "En la maqueta; vuelve a la burbuja al acabar", kSwitch},
    {kXr, "auto_screen", "Pantalla automática en 1ª persona", "Telescopio, apuntar, cámara: en 2D", kSwitch},
    {kXr, "birds_free", "Aves fuera de la burbuja", "Las gaviotas vuelan por tu habitación", kSwitch},
    {kXr, "bird_range", "Alcance de las aves", "Hasta cuántas veces el radio de la burbuja", kStepper, nullptr, 0, 1.0f,
     8.0f, 0.5f, "%.1f"},
    {kXr, "window_width", "Tamaño de la ventana", "Hasta 2,2 m; más de 1,6 m baja la resolución", kStepper, nullptr,
     0, 0.3f, 2.2f, 0.1f, "%.1f m"},
    {kXr, "window_depth", "Fondo de la ventana", "La cámara del juego delante; tope de lo que sale", kStepper, nullptr, 0, 0.1f, 2.0f,
     0.1f, "%.1f m"},
};
const Item kDiorama[] = {
    {kXr, "shape", "Forma", "Una burbuja, o una mesa", kStepper, OPTS(kShape)},
    {kXr, "bubble_size", "Tamaño", "O agárrala con los dos grips", kStepper, nullptr, 0, 0.4f, 3.0f, 0.1f, "%.1f m"},
    {kXr, "bubble_span", "Mundo dentro", "Metros del juego a lo ancho; más = más GPU", kStepper, nullptr, 0, 10.0f,
     120.0f, 5.0f, "%.0f m"},
    {kXr, "bubble_distance", "Distancia", "O muévela con un grip", kStepper, nullptr, 0, 0.3f, 2.5f, 0.05f, "%.2f m"},
    {kXr, "bubble_height", "Altura", "Link por debajo de tus ojos", kStepper, nullptr, 0, -0.6f, 1.2f, 0.05f,
     "%.2f m"},
    {kXr, "bubble_fade", "Fundido del borde", "Lo suave que se funde con la habitación", kStepper, nullptr, 0, 0.0f,
     0.5f, 0.05f, "%.2f m"},
    {kXr, "hud_width", "Tamaño del HUD", "El panel con los corazones y el mapa", kStepper, nullptr, 0, 0.6f, 2.5f,
     0.1f, "%.1f m"},
    {kXr, "dialogue_back", "Distancia de los diálogos", "Detrás del centro de la burbuja", kStepper, nullptr, 0, 0.0f,
     1.5f, 0.1f, "%.1f m"},
    {kAction, "reset_panels", "Restablecer paneles", "El HUD y los diálogos, a su sitio", kButton},
    {kXr, "window_hud_on", "HUD sobre la ventana", "Si no, en paneles debajo y encima", kSwitch},
};
const Item kControls[] = {
    {kXr, "table_follow_yaw", "Seguir la cámara", "La burbuja gira con la cámara del juego", kSwitch},
    {kXr, "view_relative", "Stick según tu vista", "Adelante = lejos de ti, como la miras", kSwitch},
    {kGame, "COS_CAMERA_INVERT_X", "Invertir cámara horizontal", "El stick derecho", kSwitch},
    {kGame, "COS_CAMERA_INVERT_Y", "Invertir cámara vertical", "El stick derecho", kSwitch},
    {kXr, "special_scenes", "Escenas especiales en 3D real", "El Rey Helmaroc en Isla Initia, de cerca", kSwitch},
    {kAction, "reset_position", "Recolocar burbuja y ventana", "Y la pantalla (o grip + clic)", kButton},
    {kXr, "seethrough_on", "Ver a Link a través de obstáculos", "Un hueco en lo que tape a Link", kSwitch},
    {kXr, "seethrough_radius", "Tamaño del hueco", "Metros del juego alrededor de Link", kStepper, nullptr, 0, 0.4f,
     4.0f, 0.2f, "%.1f m"},
    {kXr, "window_draw_distance", "Distancia del mar / horizonte", "Ventana: más = más lejos y más GPU", kStepper,
     nullptr, 0, 200.0f, 3000.0f, 100.0f, "%.0f m"},
};
// what is drawn when the GPU does not keep up (pc_npc_distance.cpp, pc_veg_distance.cpp, pc_vr_dynres.cpp)
const Item kSmooth[] = {
    {kXr, "npc_draw_distance", "Distancia de personajes", "Más lejos no se dibujan; menos = más fluido", kStepper,
     nullptr, 0, 10.0f, 200.0f, 10.0f, "%.0f m"},
    {kXr, "vegetation_adaptive", "Vegetación adaptativa", "Si la GPU no llega, quita la más lejana", kSwitch},
    {kXr, "vegetation_min", "Vegetación mínima", "Nunca quita la que esté más cerca de Link", kStepper, nullptr, 0,
     10.0f, 100.0f, 5.0f, "%.0f m"},
    {kXr, "dynres", "Resolución dinámica", "Si aun así no llega, baja la resolución", kSwitch},
};
const Item kGameItems[] = {
    {kGame, "COS_GAME_LANGUAGE", "Idioma del juego", "Al reiniciar el juego", kStepper, OPTS(kLanguage)},
    {kGame, "COS_QOL_SKIP_INTRO", "Saltar la introducción", "Al empezar una partida nueva", kSwitch},
    {kAction, "full_menu", "Más opciones", "El menú completo del juego (con el mando)", kButton},
    // the bubble in cutscenes and dialogues (pc_vr_diorama.cpp)
    {kXr, "talk_zoom_on", "Acercar en diálogos y escenas", "La burbuja se centra en quien habla", kSwitch},
    {kXr, "talk_zoom", "Cuánto acerca", "Menos = más cerca", kStepper, nullptr, 0, 0.2f, 1.0f, 0.1f, "%.1f"},
    {kXr, "talk_screen", "Pantalla junto a la burbuja", "Los diálogos en 2D, a su izquierda", kSwitch},
    {kXr, "special_window", "Helmaroc por la ventana", "Si no, la burbuja se acerca al pájaro", kSwitch},
    // the giant screen's (General is full)
    {kXr, "passthrough", "Tu habitación", "Alrededor de la pantalla gigante", kSwitch},
    {kXr, "screen_frame", "Marco", "Cuero verde alrededor de la pantalla y la ventana", kSwitch},
    {kXr, "height", "Altura de la pantalla", "Sobre tus ojos; o con un grip", kStepper, nullptr, 0, -3.0f, 3.0f,
     0.1f, "%.1f m"},
};
// the room's ambience (mode = pantalla, pc_vr_diorama.cpp): the game's seagulls, crabs and small birds
// out of the screen
const Item kAmbient[] = {
    {kXr, "ambient", "Ambiente", "Gaviotas, cangrejos y pajarillos en tu habitación", kSwitch},
    {kXr, "ambient_scale", "Tamaño del ambiente", "Lo que sale de la pantalla", kStepper, nullptr, 0, 0.25f, 3.0f,
     0.25f, "%.2f"},
    {kXr, "ambient_range", "Alcance", "Hasta dónde se ve desde el borde de la pantalla", kStepper, nullptr, 0, 1.0f,
     10.0f, 0.5f, "%.1f m"},
    {kXr, "ambient_anchor", "Ambiente anclado", "Quieto en la habitación, o con la cámara", kStepper, OPTS(kAnchor)},
    {kXr, "ambient_move", "Mover el ambiente con los grips", "En vez de la pantalla; clic del stick: a su sitio",
     kSwitch},
};
#undef OPTS

struct Tab {
    const char* name;
    const Item* items;
    int count;
};
#define TAB(name, items) {name, items, (int)(sizeof(items) / sizeof(items[0]))}
constexpr Tab kTabs[] = {TAB("General", kGeneral), TAB("Efectos", kStereo), TAB("Maqueta", kDiorama), TAB("Controles", kControls),
                     TAB("Juego", kGameItems), TAB("Ambiente", kAmbient), TAB("Fluidez", kSmooth)};
#undef TAB
constexpr int kTabCount = (int)(sizeof(kTabs) / sizeof(kTabs[0]));
constexpr int kMaxItems = 10;
constexpr bool tabsFit(int i = 0) { return i >= kTabCount || (kTabs[i].count <= kMaxItems && tabsFit(i + 1)); }
static_assert(tabsFit(), "a tab has more rows than the panel holds (kMaxItems)");

// The game's keys, for the game thread's mirror (wq_xr_panel_game_keys)
const char* const kGameKeys[] = {"COS_VR_RESOLUTION", "COS_CAMERA_INVERT_X", "COS_CAMERA_INVERT_Y",
                                 "COS_GAME_LANGUAGE", "COS_QOL_SKIP_INTRO", nullptr};
constexpr int kGameKeyCount = (int)(sizeof(kGameKeys) / sizeof(kGameKeys[0])) - 1;

// ---- Shared with the game thread ---------------------------------------------------------------------

std::mutex sGameLock;
std::string sGameValue[kGameKeyCount];
std::vector<std::pair<std::string, std::string>> sGameChanges;
std::atomic_bool sOpenShared{false};
std::atomic_bool sFullMenuAsked{false};

int gameKeyIndex(const char* key) {
    for (int i = 0; i < kGameKeyCount; i++)
        if (!strcmp(kGameKeys[i], key)) return i;
    return -1;
}

// A change for the game thread (sGameLock held): one of the same key not yet applied is replaced
void queueGameChange(const char* key, const std::string& v) {
    bool queued = false;
    for (auto& c : sGameChanges)
        if (c.first == key) c.second = v, queued = true;
    if (!queued) sGameChanges.emplace_back(key, v);
    const int i = gameKeyIndex(key);
    if (i >= 0) sGameValue[i] = v;  // shown at once; the game thread confirms it
}

// ---- The panel ---------------------------------------------------------------------------------------

// 1.0.5: wider (the six tabs' names) and larger in the room; the controls on the right as before
const int kTexW = 900;
const float kRight = kTexW - 640.0f;  // the controls' shift from the 640-wide panel's places
const float kRowsTop = 172.0f, kRowH = 104.0f;
const int kTexH = (int)(kRowsTop + kMaxItems * kRowH + 24.0f);
const float kWidthM = 0.8f;
const float kCloseX0 = 448.0f + kRight, kCloseX1 = 608.0f + kRight, kCloseY0 = 24.0f, kCloseY1 = 70.0f;
const float kTabsY0 = 92.0f, kTabsY1 = 148.0f, kTabsX0 = 16.0f, kTabsX1 = kTexW - 16.0f;
const float kControlY = 36.0f;
const float kButtonR = 28.0f;
const float kStepMinusX = 380.0f + kRight, kStepPlusX = 580.0f + kRight;
const float kSwitchX0 = 520.0f + kRight, kSwitchX1 = 608.0f + kRight, kSwitchR = 22.0f;

// A tab's button, as wide as its name plus an even share of the rest of the row
void tabSpan(int t, float* x0, float* x1) {
    float names = 0.0f;
    for (int i = 0; i < kTabCount; i++) names += ui::textWidth(kFontSmall, kTabs[i].name);
    const float spare = std::max(0.0f, (kTabsX1 - kTabsX0 - names) / kTabCount);
    float x = kTabsX0;
    for (int i = 0; i < t; i++) x += ui::textWidth(kFontSmall, kTabs[i].name) + spare;
    *x0 = x;
    *x1 = x + ui::textWidth(kFontSmall, kTabs[t].name) + spare;
}

enum Part { kNone, kClose, kTabButton, kToggle, kMinus, kPlus, kPress };
struct Hit {
    Part part = kNone;
    int index = 0;
    bool operator==(const Hit& o) const { return part == o.part && index == o.index; }
    bool operator!=(const Hit& o) const { return !(*this == o); }
};

struct Panel {
    wqp::Host host{};
    XrSession session = XR_NULL_HANDLE;
    bool srgb = false;
    bool ready = false;
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    Canvas statics, canvas;
    std::vector<uint32_t> upload;
    bool open = false, dirty = true, drawnOnce = false;
    XrPosef pose{{0, 0, 0, 1}, {0, 0, -1}};
    int tab = 0;
    int focus = 0;  // the row the controller is on
    Hit hover, pressed;
    bool clickWas = false, owns = false;
    uint64_t pressMs = 0, repeatMs = 0;
    std::string shown;  // the values on the picture
    // the controller
    int navY = 0, navX = 0, navTab = 0;
    uint64_t navMs = 0;
    bool aWas = true, bWas = true;
    // the label
    XrSwapchain labelChain = XR_NULL_HANDLE;
    int labelW = 320;  // the label's width in its chain (as wide as its text)
    std::vector<XrSwapchainImageOpenGLESKHR> labelImages;
    std::string labelText;
};
Panel p;

float heightM() { return kWidthM * kTexH / kTexW; }

std::string gameValue(const char* key) {
    const int i = gameKeyIndex(key);
    std::lock_guard lock{sGameLock};
    return i >= 0 ? sGameValue[i] : std::string();
}

std::string itemValue(const Item& it) {
    if (it.src == kXr) return p.host.xrGet(it.key);
    if (it.src == kGame) return gameValue(it.key);
    return std::string();
}

void setValue(const Item& it, const std::string& v) {
    if (it.src == kXr) {
        p.host.xrSet(it.key, v.c_str());
    } else if (it.src == kGame) {
        std::lock_guard lock{sGameLock};
        queueGameChange(it.key, v);
    }
    char line[160];
    snprintf(line, sizeof line, "%s = %s", it.key, v.c_str());
    plog("", line);
    p.dirty = true;
}

bool isOn(const std::string& v) { return !v.empty() && v != "0" && v != "off"; }

std::string comma(std::string s) {
    for (char& c : s)
        if (c == '.') c = ',';
    return s;
}

std::string valueText(const Item& it) {
    const std::string v = itemValue(it);
    if (it.kind == kSwitch) return isOn(v) ? "Sí" : "No";
    if (it.kind == kButton) return "";
    if (it.options) {
        // the option, or the nearest number's (a value set elsewhere, between two)
        for (int i = 0; i < it.optionCount; i++)
            if (v == it.options[i].value) return it.options[i].text;
        return v.empty() ? "—" : comma(v);
    }
    char t[48];
    snprintf(t, sizeof t, it.format ? it.format : "%g", atof(v.c_str()));
    return comma(t);
}

void stepItem(const Item& it, int dir) {
    const std::string v = itemValue(it);
    if (it.kind == kSwitch) {
        setValue(it, isOn(v) ? "0" : "1");
        return;
    }
    if (it.options) {
        int at = -1;
        for (int i = 0; i < it.optionCount; i++)
            if (v == it.options[i].value) at = i;
        if (at < 0) {
            // between options (numbers): the nearest one
            float best = 1e30f;
            for (int i = 0; i < it.optionCount; i++) {
                const float d = std::fabs((float)atof(it.options[i].value) - (float)atof(v.c_str()));
                if (d < best) best = d, at = i;
            }
            if (at < 0) at = 0;
        } else {
            at += dir;
        }
        at = at < 0 ? 0 : at >= it.optionCount ? it.optionCount - 1 : at;
        if (v != it.options[at].value) setValue(it, it.options[at].value);
        return;
    }
    float f = (float)atof(v.c_str()) + dir * it.step;
    f = std::round(f / it.step) * it.step;
    f = f < it.min ? it.min : f > it.max ? it.max : f;
    char t[32];
    snprintf(t, sizeof t, "%.3g", f);
    if (std::fabs(f) < 1e-4f) snprintf(t, sizeof t, "0");
    setValue(it, t);
}

void press(const Item& it) {
    if (it.kind != kButton) {
        stepItem(it, 1);
        return;
    }
    if (!strcmp(it.key, "reset_position")) {
        p.host.resetPosition();
        plog("the model back in its place");
    } else if (!strcmp(it.key, "reset_panels")) {
        p.host.resetPanels();
        plog("the HUD and the dialogues back in their places");
    } else if (!strcmp(it.key, "full_menu")) {
        sFullMenuAsked = true;
        wqp::close();
        plog("the game's full options menu");
    }
}

float rowTop(int i) { return kRowsTop + kRowH * i; }

std::string shownState() {
    std::string st = std::to_string(p.tab) + ":" + std::to_string(p.focus) + ":";
    const Tab& t = kTabs[p.tab];
    for (int i = 0; i < t.count; i++) st += valueText(t.items[i]) + "|";
    return st;
}

void drawStatic() {
    Canvas& c = p.statics;
    c.init(kTexW, kTexH);
    c.roundRect(0, 0, kTexW, kTexH, 30.0f, rgb(78, 86, 106, 0.95f));
    c.roundRect(2, 2, kTexW - 2, kTexH - 2, 28.0f, rgb(22, 25, 33));
    c.text(32, 62, kFontLarge, "Ajustes de VR", rgb(255, 255, 255));
}

void drawPanel() {
    p.canvas.px = p.statics.px;
    Canvas& c = p.canvas;
    const Color accent = rgb(86, 160, 255);
    const Color bright = rgb(196, 202, 216), dim = rgb(140, 147, 163), white = rgb(255, 255, 255),
                line = rgb(58, 63, 78);
    auto fill = [&](const Hit& h) { return p.pressed == h ? accent : p.hover == h ? rgb(84, 93, 116) : rgb(44, 49, 62); };
    const Tab& tab = kTabs[p.tab];

    c.roundRect(kCloseX0, kCloseY0, kCloseX1, kCloseY1, 23.0f, fill({kClose, 0}));
    c.text((kCloseX0 + kCloseX1) * 0.5f, 56, kFontSmall, "Cerrar", rgb(230, 234, 242), 1);
    for (int t = 0; t < kTabCount; t++) {
        float x0, x1;
        tabSpan(t, &x0, &x1);
        x0 += 3.0f, x1 -= 3.0f;
        const Hit h{kTabButton, t};
        const Color back = t == p.tab ? accent : p.pressed == h || p.hover == h ? rgb(84, 93, 116) : rgb(44, 49, 62);
        c.roundRect(x0, kTabsY0, x1, kTabsY1, 20.0f, back);
        c.text((x0 + x1) * 0.5f, kTabsY0 + 38.0f, kFontSmall, kTabs[t].name, t == p.tab ? white : bright, 1);
    }
    auto button = [&](const Hit& h, float x, float y, bool plus) {
        const float bar = 20.0f, thick = 5.0f;
        c.circle(x, y, kButtonR, fill(h));
        c.roundRect(x - bar * 0.5f, y - thick * 0.5f, x + bar * 0.5f, y + thick * 0.5f, thick * 0.5f, rgb(240, 243, 248));
        if (plus) c.roundRect(x - thick * 0.5f, y - bar * 0.5f, x + thick * 0.5f, y + bar * 0.5f, thick * 0.5f, rgb(240, 243, 248));
    };
    for (int i = 0; i < tab.count; i++) {
        const Item& it = tab.items[i];
        const float top = rowTop(i), y = top + kControlY;
        if (i == p.focus) c.roundRect(14, top + 3, kTexW - 14, top + kRowH - 3, 16.0f, rgb(34, 39, 52));
        if (i > 0) c.roundRect(32, top - 1.0f, kTexW - 32, top + 1.0f, 1.0f, line);
        c.text(32, top + 46.0f, kFontSmall, it.label, bright);
        c.text(32, top + 84.0f, kFontSmall, it.hint, dim);
        const std::string text = valueText(it);
        if (it.kind == kSwitch) {
            const Hit h{kToggle, i};
            const bool on = isOn(itemValue(it)), active = p.hover == h || p.pressed == h;
            c.roundRect(kSwitchX0, y - kSwitchR, kSwitchX1, y + kSwitchR, kSwitchR, on ? accent : active ? rgb(84, 93, 116) : line);
            c.circle(on ? kSwitchX1 - kSwitchR : kSwitchX0 + kSwitchR, y, active ? 19.0f : 17.0f, white);
            c.text(kSwitchX0 - 16, y + 10, kFontSmall, text.c_str(), rgb(230, 234, 242), 2);
        } else if (it.kind == kStepper) {
            button({kMinus, i}, kStepMinusX, y, false);
            button({kPlus, i}, kStepPlusX, y, true);
            c.text((kStepMinusX + kStepPlusX) * 0.5f, y + 10, kFontSmall, text.c_str(), white, 1);
        } else {
            const Hit h{kPress, i};
            c.roundRect(kStepMinusX - kButtonR, y - 24, kStepPlusX + kButtonR, y + 24, 24.0f, fill(h));
            c.text((kStepMinusX + kStepPlusX) * 0.5f, y + 10, kFontSmall, "Aplicar", white, 1);
        }
    }
    p.shown = shownState();
}

Hit hitAt(float x, float y) {
    auto near = [](float x, float y, float cx, float cy, float r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
    if (x >= kCloseX0 - 8 && x <= kCloseX1 + 8 && y >= kCloseY0 - 8 && y <= kCloseY1 + 8) return {kClose, 0};
    if (y >= kTabsY0 - 6 && y <= kTabsY1 + 6) {
        for (int t = 0; t < kTabCount; t++) {
            float x0, x1;
            tabSpan(t, &x0, &x1);
            if (x >= x0 && x < x1) return {kTabButton, t};
        }
    }
    const Tab& tab = kTabs[p.tab];
    for (int i = 0; i < tab.count; i++) {
        const Item& it = tab.items[i];
        const float top = rowTop(i), cy = top + kControlY;
        if (y < top + 2.0f || y > top + kRowH - 2.0f) continue;
        if (it.kind == kSwitch) return {kToggle, i};  // the whole row
        if (it.kind == kButton) {
            if (x >= kStepMinusX - kButtonR - 10 && x <= kStepPlusX + kButtonR + 10 && std::fabs(y - cy) < 34) return {kPress, i};
            continue;
        }
        if (near(x, y, kStepMinusX, cy, kButtonR + 12.0f)) return {kMinus, i};
        if (near(x, y, kStepPlusX, cy, kButtonR + 12.0f)) return {kPlus, i};
    }
    return {};
}

void act(const Hit& h) {
    const Tab& tab = kTabs[p.tab];
    switch (h.part) {
    case kClose: wqp::close(); break;
    case kTabButton:
        p.tab = h.index;
        p.focus = 0;
        break;
    case kToggle:
    case kPress: press(tab.items[h.index]); break;
    case kMinus: stepItem(tab.items[h.index], -1); break;
    case kPlus: stepItem(tab.items[h.index], 1); break;
    default: break;
    }
    if (h.part >= kToggle) p.focus = h.index;
    p.dirty = true;
}

XrSwapchain makeChain(uint32_t w, uint32_t h, std::vector<XrSwapchainImageOpenGLESKHR>& images) {
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = p.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    sc.sampleCount = 1;
    sc.width = w;
    sc.height = h;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    XrSwapchain chain = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateSwapchain(p.session, &sc, &chain))) {
        plog("xrCreateSwapchain failed");
        return XR_NULL_HANDLE;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(chain, 0, &n, nullptr);
    images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(chain, n, &n, (XrSwapchainImageBaseHeader*)images.data());
    return chain;
}

// The canvas into the next image of `chain`, rows bottom up for GL.
void uploadTo(XrSwapchain chain, const std::vector<XrSwapchainImageOpenGLESKHR>& images, const Canvas& c) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(chain, &acq, &idx)) || XR_FAILED(xrWaitSwapchainImage(chain, &wait))) return;
    p.upload.resize((size_t)c.w * c.h);
    for (int y = 0; y < c.h; y++) memcpy(&p.upload[(size_t)y * c.w], &c.px[(size_t)(c.h - 1 - y) * c.w], c.w * 4);
    glBindTexture(GL_TEXTURE_2D, images[idx].image);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, c.w, c.h, GL_RGBA, GL_UNSIGNED_BYTE, p.upload.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(chain, &rel);
}

uint64_t nowMs() { return SDL_GetTicks(); }

}  // namespace

namespace wqp {

void init(const Host& host, XrSession session, bool srgb) {
    p.host = host;
    p.session = session;
    p.srgb = srgb;
    ui::initFonts();
    drawStatic();
    p.canvas.init(kTexW, kTexH);
    p.chain = makeChain(kTexW, kTexH, p.images);
    p.labelChain = makeChain(kLabelMaxW, 72, p.labelImages);
    p.ready = p.chain != XR_NULL_HANDLE;
    plog(p.ready ? "ready" : "not available");
}

bool isOpen() { return p.open; }
XrPosef pose() { return p.pose; }
void setPose(const XrPosef& pose) { p.pose = pose; }

void open(const XrPosef& pose) {
    if (!p.ready) return;
    p.pose = pose;
    p.open = true;
    p.dirty = true;
    p.owns = false;
    p.hover = p.pressed = {};
    p.aWas = p.bWas = true;  // the buttons and the triggers that opened it are not presses
    p.clickWas = true;
    sOpenShared = true;
    plog("open");
}

void close() {
    if (!p.open) return;
    p.open = false;
    sOpenShared = false;
    plog("closed");
}

float pointer(XrVector3f o, XrVector3f d, bool clickDown, bool* tick, bool* hovering) {
    *tick = false;
    *hovering = false;
    if (!p.open) {
        p.clickWas = clickDown;
        return 0.0f;
    }
    const XrQuaternionf& q = p.pose.orientation;
    auto rot = [&](XrVector3f v) {
        const XrVector3f u{q.x, q.y, q.z};
        const XrVector3f t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
        return XrVector3f{v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                          v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
    };
    const XrVector3f right = rot({1, 0, 0}), up = rot({0, 1, 0}), normal = rot({0, 0, 1});
    const XrVector3f c = p.pose.position;
    auto dot = [](XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    float hitT = 0.0f, px = 0.0f, py = 0.0f;
    const float dn = dot(d, normal);
    if (dn < -1e-4f) {
        const XrVector3f co{c.x - o.x, c.y - o.y, c.z - o.z};
        const float t = dot(co, normal) / dn;
        const XrVector3f h{o.x + d.x * t - c.x, o.y + d.y * t - c.y, o.z + d.z * t - c.z};
        const float u = dot(h, right) / kWidthM + 0.5f, v = dot(h, up) / heightM() + 0.5f;
        if (t > 0.0f && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            hitT = t;
            px = u * kTexW;
            py = (1.0f - v) * kTexH;
        }
    }
    const Hit over = hitT > 0.0f ? hitAt(px, py) : Hit{};
    if (over != p.hover) {
        p.hover = over;
        p.dirty = true;
        if (over.part != kNone) *tick = true;
        if (over.part >= kToggle) p.focus = over.index;
    }
    *hovering = over.part != kNone;
    const uint64_t now = nowMs();
    if (clickDown && !p.clickWas && hitT > 0.0f) {
        p.owns = true;
        p.pressed = over;
        p.pressMs = p.repeatMs = now;
        *tick = true;
        act(over);
    }
    if (p.owns && clickDown && (p.pressed.part == kMinus || p.pressed.part == kPlus) && over == p.pressed &&
        now - p.pressMs > 450 && now - p.repeatMs > 130) {
        p.repeatMs = now;
        stepItem(kTabs[p.tab].items[p.pressed.index], p.pressed.part == kPlus ? 1 : -1);
    }
    if (!clickDown) {
        if (p.pressed.part != kNone) p.dirty = true;
        p.owns = false;
        p.pressed = {};
    }
    p.clickWas = clickDown;
    return hitT;
}

void nav(float sx, float sy, float tx, bool a, bool b, bool* tick) {
    *tick = false;
    if (!p.open) return;
    const uint64_t now = nowMs();
    const int ny = sy > 0.6f ? -1 : sy < -0.6f ? 1 : 0;  // up the list is a smaller index
    const int nx = sx > 0.6f ? 1 : sx < -0.6f ? -1 : 0;
    const int nt = tx > 0.6f ? 1 : tx < -0.6f ? -1 : 0;
    auto repeat = [&](int v, int& was) {
        if (v == 0) {
            was = 0;
            return false;
        }
        if (v != was) {
            was = v;
            p.navMs = now + 350;
            return true;
        }
        if (now >= p.navMs) {
            p.navMs = now + 120;
            return true;
        }
        return false;
    };
    const Tab& tab = kTabs[p.tab];
    if (repeat(ny, p.navY)) {
        const int f = p.focus + ny;
        if (f >= 0 && f < tab.count) p.focus = f, *tick = true, p.dirty = true;
    }
    if (repeat(nx, p.navX) && p.focus < tab.count) {
        const Item& it = tab.items[p.focus];
        if (it.kind == kStepper) stepItem(it, nx), *tick = true;
        else if (it.kind == kSwitch && isOn(itemValue(it)) != (nx > 0)) stepItem(it, nx), *tick = true;
    }
    if (nt != p.navTab) {
        p.navTab = nt;
        if (nt) {
            p.tab = (p.tab + nt + kTabCount) % kTabCount;
            p.focus = 0;
            p.dirty = true;
            *tick = true;
        }
    }
    if (a && !p.aWas && p.focus < tab.count) {
        press(tab.items[p.focus]);
        *tick = true;
    }
    p.aWas = a;
    if (b && !p.bWas) close();
    p.bWas = b;
}

bool layer(XrSpace space, XrCompositionLayerQuad* out) {
    if (!p.open || !p.ready) return false;
    if (!p.dirty && shownState() != p.shown) p.dirty = true;  // a value changed elsewhere (the game's)
    if (p.dirty || !p.drawnOnce) {
        drawPanel();
        uploadTo(p.chain, p.images, p.canvas);
        p.dirty = false;
        p.drawnOnce = true;
    }
    *out = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = space;
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = p.chain;
    out->subImage.imageRect = {{0, 0}, {kTexW, kTexH}};
    out->pose = p.pose;
    out->size = {kWidthM, heightM()};
    return true;
}

bool label(const char* text, const XrPosef& pose, XrSpace space, XrCompositionLayerQuad* out) {
    if (!p.labelChain || !text || !text[0]) return false;
    if (p.labelText != text) {
        p.labelText = text;
        // as wide as the text (at least 320), within the chain's width
        p.labelW = std::min(kLabelMaxW, std::max(320, int(ui::textWidth(kFontLarge, text)) + 72));
        Canvas c;
        c.init(kLabelMaxW, 72);
        c.roundRect(0, 0, float(p.labelW), 72, 36.0f, rgb(22, 25, 33, 0.85f));
        c.text(0.5f * p.labelW, 50, kFontLarge, text, rgb(255, 255, 255), 1);
        uploadTo(p.labelChain, p.labelImages, c);
    }
    *out = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = space;
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = p.labelChain;
    out->subImage.imageRect = {{0, 0}, {p.labelW, 72}};
    out->pose = pose;
    out->size = {0.16f * float(p.labelW) / 320.0f, 0.036f};
    return true;
}

void gameChange(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) return;
    std::lock_guard lock{sGameLock};
    queueGameChange(key, value);
}

}  // namespace wqp

// ---- The game thread ---------------------------------------------------------------------------------

extern "C" const char* const* wq_xr_panel_game_keys(void) { return kGameKeys; }

extern "C" void wq_xr_panel_game_value(const char* key, const char* value) {
    const int i = gameKeyIndex(key);
    if (i < 0) return;
    std::lock_guard lock{sGameLock};
    // a change not yet applied keeps showing the panel's value
    for (const auto& c : sGameChanges)
        if (c.first == key) return;
    sGameValue[i] = value ? value : "";
}

extern "C" bool wq_xr_panel_take_change(char* key, int keySize, char* value, int valueSize) {
    std::lock_guard lock{sGameLock};
    if (sGameChanges.empty()) return false;
    snprintf(key, keySize, "%s", sGameChanges.front().first.c_str());
    snprintf(value, valueSize, "%s", sGameChanges.front().second.c_str());
    sGameChanges.erase(sGameChanges.begin());
    return true;
}

extern "C" int wq_xr_panel_flags(void) {
    return (sOpenShared.load() ? 1 : 0) | (sFullMenuAsked.exchange(false) ? 2 : 0);
}
