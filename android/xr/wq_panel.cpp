// WakerQuest's settings panel in the headset, after GalaxyQuest's VR settings panel
// (platform/src/xr/vr_settings.cpp): a panel drawn by the headset layer on the CPU (ui_canvas.h) into
// a quad layer of its own, sharp whatever the game's resolution, in front of the player in any mode
// (the giant screen, the diorama, the window). It opens and closes with the left stick's click (fase 2;
// before, both triggers and the right grip, which the grips now need), and is worked with the laser
// pointer (either hand's trigger) or the controller. The game waits while it is open
// (pc_vr_panel_frame, the game thread).
//
// Its settings are of two kinds: xr.ini's (the headset layer's own, set at once through the Host),
// and a few of the game's (pc_settings: the language, the camera...), which the game thread mirrors
// every frame and applies (wq_xr_panel_* in wq_xr.h). The other game settings stay in the game's
// options menu, a button away ("Más opciones").
//
// Fase 2: nine tabs in two rows, by what they set (General, Pantalla, Maqueta, Ventana, Ambiente;
// Escenas, Rendimiento, Mandos, Acerca de), every row of before kept; a few rows that only tell (the
// controls) and the Acerca de tab (the version, the update check). In three languages (xr.ini
// language = es, en or fr): each text a Tr, the French letters in ui_font.h since then.
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

// ---- The languages -----------------------------------------------------------------------------------

// A text in the panel's three languages: Spanish, English, French (nullptr: the Spanish one, for numbers
// and names that do not change)
struct Tr {
    const char* es;
    const char* en;
    const char* fr;
};

int sLang = 0;  // xr.ini language: 0 es, 1 en, 2 fr (read when the panel is drawn)

const char* tr(const Tr& t) {
    const char* s = sLang == 1 ? t.en : sLang == 2 ? t.fr : t.es;
    return s != nullptr ? s : t.es;
}

int languageOf(const std::string& v) { return v == "en" ? 1 : v == "fr" ? 2 : 0; }

// ---- The settings ------------------------------------------------------------------------------------

enum Source { kXr, kGame, kAction };
enum Kind { kSwitch, kStepper, kButton, kInfo };  // kInfo: a row that only tells (a value at most)

struct Option {
    const char* value;
    Tr text;
};

struct Item {
    Source src;
    const char* key;  // xr.ini's or pc_settings'; for kAction, what it does
    Tr label;
    Tr hint;
    Kind kind;
    const Option* options;  // the values to step through, or a number range:
    int optionCount;
    float min, max, step;
    const char* format;     // of the number ("%.1f m"); the decimal point shown as a comma (es, fr)
};

constexpr int kLabelMaxW = 640;  // the size label's chain (wq_xr.cpp: the window's line is longer)

#define OPTS(o) o, (int)(sizeof(o) / sizeof(o[0]))
const Option kMode[] = {{"pantalla", {"Pantalla", "Screen", "Écran"}},
                        {"maqueta", {"Maqueta", "Diorama", "Maquette"}},
                        {"ventana", {"Ventana", "Window", "Fenêtre"}}};
const Option kShape[] = {{"esfera", {"Esfera", "Sphere", "Sphère"}}, {"caja", {"Caja", "Box", "Boîte"}}};
const Option kRefresh[] = {{"72", {"72 Hz"}}, {"80", {"80 Hz"}}, {"90", {"90 Hz"}}, {"120", {"120 Hz"}}};
const Option kResolution[] = {{"540", {"0,50", "0.50"}},   {"648", {"0,60", "0.60"}},   {"756", {"0,70", "0.70"}},
                              {"864", {"0,80", "0.80"}},   {"918", {"0,85", "0.85"}},   {"972", {"0,90", "0.90"}},
                              {"1080", {"1,00", "1.00"}},  {"1188", {"1,10", "1.10"}},  {"1296", {"1,20", "1.20"}},
                              {"1404", {"1,30", "1.30"}},  {"1512", {"1,40", "1.40"}},  {"1620", {"1,50", "1.50"}},
                              {"1728", {"1,60", "1.60"}},  {"1944", {"1,80", "1.80"}},  {"2160", {"2,00", "2.00"}}};
// the Rendimiento preset (wq_xr.cpp kPresetKeys): refresh, GPU, resolution, 3D, the bubble's world, the
// window's horizon, the characters' distance, set at once when one is chosen; personal = the values chosen by hand
const Option kPerfPreset[] = {{"ahorro", {"Ahorro", "Saving", "Économie"}},
                              {"equilibrado", {"Equilibrado", "Balanced", "Équilibré"}},
                              {"maximo", {"Máximo", "Maximum", "Maximum"}},
                              {"personal", {"Personal", "Custom", "Perso"}}};
const Option kAnchor[] = {{"world", {"Habitación", "Room", "Pièce"}}, {"camera", {"Cámara", "Camera", "Caméra"}}};
const Option kLanguage[] = {{"auto", {"Auto"}},       {"es", {"Español"}}, {"en", {"English"}},
                            {"fr", {"Français"}},     {"de", {"Deutsch"}}, {"it", {"Italiano"}}};
const Option kPanelLanguage[] = {{"es", {"Español"}}, {"en", {"English"}}, {"fr", {"Français"}}};

// General: the mode, the languages, the game's start
const Item kGeneral[] = {
    {kXr, "mode", {"Modo", "Mode", "Mode"},
     {"Pantalla gigante, el mundo en burbuja, o ventana", "A giant screen, the world in a bubble, or a window",
      "Écran géant, le monde en bulle, ou fenêtre"},
     kStepper, OPTS(kMode)},
    {kXr, "intro_2d", {"Título en 2D", "Title screen in 2D", "Écran titre en 2D"},
     {"La pantalla de título, en pantalla plana", "The title screen, on the flat screen",
      "L'écran titre, sur écran plat"},
     kSwitch},
    {kXr, "language", {"Idioma del panel", "Panel language", "Langue du panneau"},
     {"Este panel de ajustes", "This settings panel", "Ce panneau de réglages"}, kStepper, OPTS(kPanelLanguage)},
    {kGame, "COS_GAME_LANGUAGE", {"Idioma del juego", "Game language", "Langue du jeu"},
     {"Al reiniciar el juego", "When the game starts again", "Au redémarrage du jeu"}, kStepper, OPTS(kLanguage)},
    {kGame, "COS_QOL_SKIP_INTRO", {"Saltar la introducción", "Skip the intro", "Passer l'introduction"},
     {"Al empezar una partida nueva", "When a new game starts", "En commençant une nouvelle partie"}, kSwitch},
    {kAction, "full_menu", {"Más opciones", "More options", "Plus d'options"},
     {"El menú completo del juego (con el mando)", "The game's full menu (with the controller)",
      "Le menu complet du jeu (à la manette)"},
     kButton},
    {kAction, "reset_position", {"Recolocar burbuja y ventana", "Reset bubble and window", "Replacer bulle et fenêtre"},
     {"Y la pantalla (o grip + clic)", "And the screen (or grip + click)", "Et l'écran (ou grip + clic)"}, kButton},
};
// the giant screen (mode = pantalla): also moved with a grip and scaled with both (wq_xr.cpp), and its 3D
const Item kScreen[] = {
    {kXr, "distance", {"Distancia de la pantalla", "Screen distance", "Distance de l'écran"},
     {"O muévela con un grip", "Or move it with a grip", "Ou déplacez-le avec un grip"}, kStepper, nullptr, 0, 0.5f,
     30.0f, 0.25f, "%.2f m"},
    {kXr, "width", {"Tamaño de la pantalla", "Screen size", "Taille de l'écran"},
     {"O agárrala con los dos grips", "Or hold it with both grips", "Ou tenez-le avec les deux grips"}, kStepper,
     nullptr, 0, 0.5f, 40.0f, 0.25f, "%.2f m"},
    {kXr, "height", {"Altura de la pantalla", "Screen height", "Hauteur de l'écran"},
     {"Sobre tus ojos; o con un grip", "Above your eyes; or with a grip", "Au-dessus des yeux ; ou avec un grip"},
     kStepper, nullptr, 0, -3.0f, 3.0f, 0.1f, "%.1f m"},
    {kXr, "passthrough", {"Tu habitación", "Your room", "Votre pièce"},
     {"Alrededor de la pantalla gigante", "Around the giant screen", "Autour de l'écran géant"}, kSwitch},
    {kXr, "screen_frame", {"Marco", "Frame", "Cadre"},
     {"Cuero verde alrededor de la pantalla y la ventana", "Green leather around the screen and the window",
      "Cuir vert autour de l'écran et de la fenêtre"},
     kSwitch},
    {kXr, "stereo", {"3D en la pantalla", "3D on the screen", "3D sur l'écran"},
     {"Una imagen para cada ojo", "A picture for each eye", "Une image pour chaque œil"}, kSwitch},
    {kXr, "stereo_strength", {"Profundidad 3D", "3D depth", "Profondeur 3D"},
     {"Más = el fondo más lejos", "More = the background farther", "Plus = le fond plus loin"}, kStepper, nullptr, 0,
     0.004f, 0.10f, 0.002f, "%.3f"},
    {kXr, "stereo_focus", {"Salir de la pantalla", "Out of the screen", "Sortir de l'écran"},
     {"Más = lo cercano sale hacia ti", "More = near things come out at you", "Plus = le proche sort vers vous"},
     kStepper, nullptr, 0, 50.0f, 2500.0f, 100.0f, "%.0f"},
    {kXr, "cine_3d", {"3D intenso en cinemáticas", "Stronger 3D in cutscenes", "3D intense en cinématiques"},
     {"Más profundidad en las escenas", "More depth in the scenes", "Plus de profondeur dans les scènes"}, kSwitch},
};
// the diorama (mode = maqueta): the bubble or the table, its birds, the hole around Link
const Item kDiorama[] = {
    {kXr, "shape", {"Forma", "Shape", "Forme"}, {"Una burbuja, o una mesa", "A bubble, or a table", "Une bulle, ou une table"},
     kStepper, OPTS(kShape)},
    {kXr, "bubble_size", {"Tamaño", "Size", "Taille"},
     {"O agárrala con los dos grips", "Or hold it with both grips", "Ou tenez-la avec les deux grips"}, kStepper,
     nullptr, 0, 0.4f, 3.0f, 0.1f, "%.1f m"},
    {kXr, "bubble_span", {"Mundo dentro", "World inside", "Monde dedans"},
     {"Metros del juego a lo ancho; o grip + stick", "Game metres across; or grip + stick",
      "Mètres du jeu en largeur ; ou grip + stick"},
     kStepper, nullptr, 0, 5.0f, 300.0f, 5.0f, "%.0f m"},
    {kXr, "bubble_distance", {"Distancia", "Distance", "Distance"},
     {"O muévela con un grip", "Or move it with a grip", "Ou déplacez-la avec un grip"}, kStepper, nullptr, 0, 0.3f,
     2.5f, 0.05f, "%.2f m"},
    {kXr, "bubble_height", {"Altura", "Height", "Hauteur"},
     {"Link por debajo de tus ojos", "Link below your eyes", "Link sous vos yeux"}, kStepper, nullptr, 0, -0.6f, 1.2f,
     0.05f, "%.2f m"},
    {kXr, "bubble_fade", {"Fundido del borde", "Edge fade", "Fondu du bord"},
     {"Lo suave que se funde con la habitación", "How softly it blends into the room",
      "La douceur du fondu avec la pièce"},
     kStepper, nullptr, 0, 0.0f, 0.5f, 0.05f, "%.2f m"},
    {kXr, "birds_free", {"Aves fuera de la burbuja", "Birds outside the bubble", "Oiseaux hors de la bulle"},
     {"Las gaviotas vuelan por tu habitación", "The seagulls fly around your room", "Les mouettes volent dans la pièce"},
     kSwitch},
    {kXr, "bird_range", {"Alcance de las aves", "Birds' reach", "Portée des oiseaux"},
     {"Hasta cuántas veces el radio de la burbuja", "Up to how many times the bubble's radius",
      "Jusqu'à combien de fois le rayon de la bulle"},
     kStepper, nullptr, 0, 1.0f, 8.0f, 0.5f, "%.1f"},
    {kXr, "seethrough_on", {"Ver a Link a través de obstáculos", "See Link through obstacles", "Voir Link à travers les obstacles"},
     {"Un hueco en lo que tape a Link", "A hole in whatever hides Link", "Un trou dans ce qui cache Link"}, kSwitch},
    {kXr, "seethrough_radius", {"Tamaño del hueco", "Hole size", "Taille du trou"},
     {"Metros del juego alrededor de Link", "Game metres around Link", "Mètres du jeu autour de Link"}, kStepper,
     nullptr, 0, 0.4f, 4.0f, 0.2f, "%.1f m"},
};
// the window (mode = ventana): its size and place also with the grips
const Item kWindow[] = {
    {kXr, "window_width", {"Tamaño de la ventana", "Window size", "Taille de la fenêtre"},
     {"Hasta 2,2 m; más de 1,6 m baja la resolución", "Up to 2.2 m; over 1.6 m, a lower resolution",
      "Jusqu'à 2,2 m ; après 1,6 m, moins net"},
     kStepper, nullptr, 0, 0.3f, 2.2f, 0.1f, "%.1f m"},
    {kXr, "window_scale", {"Escala de la ventana", "Window scale", "Échelle de la fenêtre"},
     {"Más = el mundo más pequeño; o grip + stick", "More = a smaller world; or grip + stick",
      "Plus = un monde plus petit ; ou grip + stick"},
     kStepper, nullptr, 0, 0.25f, 20.0f, 0.25f, "%.2f"},
    {kXr, "window_depth", {"Fondo de la ventana", "Window depth", "Profondeur de la fenêtre"},
     {"La cámara del juego delante; tope de lo que sale", "The game camera in front; caps what comes out",
      "La caméra du jeu devant ; limite ce qui sort"},
     kStepper, nullptr, 0, 0.1f, 2.0f, 0.1f, "%.1f m"},
    {kXr, "window_popout_on", {"Salir de la ventana", "Out of the window", "Sortir de la fenêtre"},
     {"Lo cercano sale hacia ti", "Near things come out at you", "Le proche sort vers vous"}, kSwitch},
    {kXr, "window_popout", {"Cuánto sale", "How far out", "Jusqu'où il sort"},
     {"Metros por delante de la ventana", "Metres in front of the window", "Mètres devant la fenêtre"}, kStepper,
     nullptr, 0, 0.1f, 2.0f, 0.1f, "%.1f m"},
    {kXr, "window_draw_distance", {"Distancia del mar / horizonte", "Sea / horizon distance", "Distance mer / horizon"},
     {"Más = más lejos y más GPU", "More = farther and more GPU", "Plus = plus loin et plus de GPU"}, kStepper,
     nullptr, 0, 200.0f, 3000.0f, 100.0f, "%.0f m"},
    {kXr, "window_hud_on", {"HUD sobre la ventana", "HUD over the window", "HUD sur la fenêtre"},
     {"Si no, en paneles debajo y encima", "Else on panels below and above", "Sinon sur des panneaux dessous et dessus"},
     kSwitch},
};
// the room's ambience (mode = pantalla, pc_vr_diorama.cpp): the game's seagulls, crabs and small birds
// out of the screen
const Item kAmbient[] = {
    {kXr, "ambient", {"Ambiente", "Ambience", "Ambiance"},
     {"Gaviotas, cangrejos y pajarillos en tu habitación", "Seagulls, crabs and small birds in your room",
      "Mouettes, crabes et oiseaux dans votre pièce"},
     kSwitch},
    {kXr, "ambient_scale", {"Tamaño del ambiente", "Ambience size", "Taille de l'ambiance"},
     {"Lo que sale de la pantalla; o grip + stick", "What comes out of the screen; or grip + stick",
      "Ce qui sort de l'écran ; ou grip + stick"},
     kStepper, nullptr, 0, 0.1f, 10.0f, 0.1f, "%.2f"},
    {kXr, "ambient_range", {"Alcance", "Reach", "Portée"},
     {"Hasta dónde se ve desde el borde de la pantalla", "How far from the screen's edge it shows",
      "Jusqu'où on le voit depuis le bord de l'écran"},
     kStepper, nullptr, 0, 1.0f, 10.0f, 0.5f, "%.1f m"},
    {kXr, "ambient_anchor", {"Ambiente anclado", "Ambience anchored", "Ambiance ancrée"},
     {"Quieto en la habitación, o con la cámara", "Still in the room, or with the camera",
      "Fixe dans la pièce, ou avec la caméra"},
     kStepper, OPTS(kAnchor)},
    {kXr, "ambient_move", {"Mover el ambiente con los grips", "Move the ambience with the grips", "Ambiance aux grips"},
     {"En vez de la pantalla; clic del stick: a su sitio", "Instead of the screen; stick click: back in place",
      "Au lieu de l'écran ; clic du stick : à sa place"},
     kSwitch},
};
// cutscenes, dialogues and the diorama's panels (pc_vr_diorama.cpp)
const Item kScenes[] = {
    {kXr, "cine_screen", {"Cinemáticas en pantalla gigante", "Cutscenes on the giant screen", "Cinématiques sur écran géant"},
     {"En la maqueta; vuelve a la burbuja al acabar", "In the diorama; back to the bubble after",
      "Dans la maquette ; retour à la bulle ensuite"},
     kSwitch},
    {kXr, "auto_screen", {"Pantalla automática en 1ª persona", "Automatic screen in first person", "Écran auto en vue subjective"},
     {"Telescopio, apuntar, cámara: en 2D", "Telescope, aiming, camera: in 2D", "Longue-vue, visée, photo : en 2D"},
     kSwitch},
    {kXr, "talk_zoom_on", {"Acercar en diálogos y escenas", "Zoom in on dialogues and scenes", "Zoom en dialogues et scènes"},
     {"La burbuja se centra en quien habla", "The bubble centres on who talks", "La bulle se centre sur qui parle"},
     kSwitch},
    {kXr, "talk_zoom", {"Cuánto acerca", "How much it zooms", "Force du zoom"},
     {"Menos = más cerca", "Less = closer", "Moins = plus près"}, kStepper, nullptr, 0, 0.2f, 1.0f, 0.1f, "%.1f"},
    {kXr, "talk_screen", {"Pantalla junto a la burbuja", "Screen beside the bubble", "Écran à côté de la bulle"},
     {"Los diálogos en 2D, a su izquierda", "The dialogues in 2D, on its left", "Les dialogues en 2D, à sa gauche"},
     kSwitch},
    {kXr, "special_scenes", {"Escenas especiales en 3D real", "Special scenes in real 3D", "Scènes spéciales en vraie 3D"},
     {"El Rey Helmaroc en Isla Initia, de cerca", "The Helmaroc King on Outset, up close",
      "Le Roi Helmaroc sur l'île de l'Aurore"},
     kSwitch},
    {kXr, "special_window", {"Helmaroc por la ventana", "Helmaroc through the window", "Helmaroc par la fenêtre"},
     {"Si no, la burbuja se acerca al pájaro", "Else the bubble closes in on the bird", "Sinon la bulle s'approche de l'oiseau"},
     kSwitch},
    {kXr, "hud_width", {"Tamaño del HUD", "HUD size", "Taille du HUD"},
     {"El panel con los corazones y el mapa", "The panel with the hearts and the map",
      "Le panneau avec les cœurs et la carte"},
     kStepper, nullptr, 0, 0.6f, 2.5f, 0.1f, "%.1f m"},
    {kXr, "dialogue_back", {"Distancia de los diálogos", "Dialogue distance", "Distance des dialogues"},
     {"Detrás del centro de la burbuja", "Behind the bubble's middle", "Derrière le centre de la bulle"}, kStepper,
     nullptr, 0, 0.0f, 1.5f, 0.1f, "%.1f m"},
    {kAction, "reset_panels", {"Restablecer paneles", "Reset panels", "Replacer les panneaux"},
     {"El HUD y los diálogos, a su sitio", "The HUD and the dialogues, back in place", "Le HUD et les dialogues à leur place"},
     kButton},
};
// the preset and what is drawn when the GPU does not keep up (pc_npc_distance.cpp, pc_veg_distance.cpp,
// pc_vr_dynres.cpp)
const Item kPerformance[] = {
    {kXr, "perf_preset", {"Rendimiento", "Performance", "Performances"},
     {"Ahorro: menos calor; Máximo: más nitidez y 3D", "Saving: less heat; Maximum: sharper, 3D",
      "Économie : moins chaud ; Maximum : plus net, 3D"},
     kStepper, OPTS(kPerfPreset)},
    {kGame, "COS_VR_RESOLUTION", {"Resolución", "Resolution", "Résolution"},
     {"La de este modo (y del 3D); 1,00 = 1080 líneas", "This mode's (and the 3D's); 1.00 = 1080 lines",
      "Celle de ce mode (et de la 3D) ; 1,00 = 1080 lignes"},
     kStepper, OPTS(kResolution)},
    {kXr, "refresh", {"Frecuencia", "Refresh rate", "Fréquence"},
     {"72 Hz: menos calor; 90 Hz: más fluido", "72 Hz: less heat; 90 Hz: smoother", "72 Hz : moins chaud ; 90 Hz : plus fluide"},
     kStepper, OPTS(kRefresh)},
    {kXr, "gpu_boost", {"Más potencia de GPU", "More GPU power", "Plus de puissance GPU"},
     {"Sube el reloj de la GPU; gasta más batería", "Raises the GPU's clock; uses more battery",
      "Horloge du GPU plus haute ; plus de batterie"},
     kSwitch},
    {kXr, "dynres", {"Resolución dinámica", "Dynamic resolution", "Résolution dynamique"},
     {"Si aun así no llega, baja la resolución", "If it still cannot keep up, a lower resolution",
      "Si ça ne suit toujours pas, baisse la résolution"},
     kSwitch},
    {kXr, "npc_draw_distance", {"Distancia de personajes", "Character distance", "Distance des personnages"},
     {"Más lejos no se dibujan; menos = más fluido", "Farther ones are not drawn; less = smoother",
      "Plus loin, non dessinés ; moins = plus fluide"},
     kStepper, nullptr, 0, 10.0f, 200.0f, 10.0f, "%.0f m"},
    {kXr, "vegetation_adaptive", {"Vegetación adaptativa", "Adaptive vegetation", "Végétation adaptative"},
     {"Si baja de 30 fps, quita la más lejana", "Under 30 fps, the farthest goes first", "Sous 30 fps, retire la plus lointaine"},
     kSwitch},
    {kXr, "vegetation_min", {"Vegetación mínima", "Minimum vegetation", "Végétation minimale"},
     {"Nunca quita la que esté más cerca de Link", "Never what is nearer to Link", "Jamais celle plus près de Link"},
     kStepper, nullptr, 0, 10.0f, 100.0f, 5.0f, "%.0f m"},
    {kXr, "palm_draw_distance", {"Distancia de palmeras", "Palm tree distance", "Distance des palmiers"},
     {"Más lejos no se dibujan; menos = más fluido", "Farther ones are not drawn; less = smoother",
      "Plus loin, non dessinés ; moins = plus fluide"},
     kStepper, nullptr, 0, 20.0f, 200.0f, 10.0f, "%.0f m"},
};
// the controls: the stick and the camera, and how the headset's own buttons work (wq_xr.cpp, fase 2)
const Item kControls[] = {
    {kXr, "table_follow_yaw", {"Seguir la cámara", "Follow the camera", "Suivre la caméra"},
     {"La burbuja gira con la cámara del juego", "The bubble turns with the game camera",
      "La bulle tourne avec la caméra du jeu"},
     kSwitch},
    {kXr, "view_relative", {"Stick según tu vista", "Stick by your view", "Stick selon votre vue"},
     {"Adelante = lejos de ti, como la miras", "Forward = away from you, as you look", "Avant = loin de vous, selon votre vue"},
     kSwitch},
    {kGame, "COS_CAMERA_INVERT_X", {"Invertir cámara horizontal", "Invert camera horizontally", "Caméra inversée (horizontal)"},
     {"El stick derecho", "The right stick", "Le stick droit"}, kSwitch},
    {kGame, "COS_CAMERA_INVERT_Y", {"Invertir cámara vertical", "Invert camera vertically", "Caméra inversée (vertical)"},
     {"El stick derecho", "The right stick", "Le stick droit"}, kSwitch},
    {kAction, "", {"Menús: clic del stick izquierdo", "Menus: left stick click", "Menus : clic du stick gauche"},
     {"Abre y cierra este panel; cierra el menú del juego", "Opens or closes this panel; closes the game's menu",
      "Ouvre et ferme ce panneau ; ferme le menu du jeu"},
     kInfo},
    {kAction, "", {"Grips: agarrar", "Grips: hold", "Grips : saisir"},
     {"Uno mueve y gira; los dos cambian el tamaño", "One moves and turns; both resize",
      "Un déplace et tourne ; les deux redimensionnent"},
     kInfo},
    {kAction, "", {"Grip + stick derecho: escala", "Grip + right stick: scale", "Grip + stick droit : échelle"},
     {"Del mundo (maqueta, ventana) o del ambiente", "Of the world (diorama, window) or the ambience",
      "Du monde (maquette, fenêtre) ou de l'ambiance"},
     kInfo},
    {kAction, "", {"Clic del stick derecho: Z", "Right stick click: Z", "Clic du stick droit : Z"},
     {"Con un grip pulsado: lo agarrado, a su sitio", "With a grip held: what it holds, back in place",
      "Avec un grip serré : l'objet à sa place"},
     kInfo},
};
// the version and the update check (the Host's info and checkUpdate: WakerQuestActivity, GitHub)
const Item kAbout[] = {
    {kAction, "version", {"Versión", "Version", "Version"},
     {"WakerQuest instalado en tus gafas", "WakerQuest on your headset", "WakerQuest sur votre casque"}, kInfo},
    {kAction, "check_update", {"Buscar actualización", "Check for updates", "Mise à jour"},
     {"Compara con la última en GitHub; no descarga nada", "Compares with GitHub's latest; downloads nothing",
      "Compare avec GitHub ; ne télécharge rien"},
     kButton},
};
#undef OPTS

struct Tab {
    Tr name;
    const Item* items;
    int count;
};
#define TAB(es, en, fr, items) {{es, en, fr}, items, (int)(sizeof(items) / sizeof(items[0]))}
constexpr Tab kTabs[] = {TAB("General", "General", "Général", kGeneral),
                         TAB("Pantalla", "Screen", "Écran", kScreen),
                         TAB("Maqueta", "Diorama", "Maquette", kDiorama),
                         TAB("Ventana", "Window", "Fenêtre", kWindow),
                         TAB("Ambiente", "Ambience", "Ambiance", kAmbient),
                         TAB("Escenas", "Scenes", "Scènes", kScenes),
                         TAB("Rendimiento", "Performance", "Performances", kPerformance),
                         TAB("Mandos", "Controls", "Commandes", kControls),
                         TAB("Acerca de", "About", "À propos", kAbout)};
#undef TAB
constexpr int kTabCount = (int)(sizeof(kTabs) / sizeof(kTabs[0]));
constexpr int kTabsFirstRow = 5;  // the tabs in two rows: five, then the rest
constexpr int kMaxItems = 10;
constexpr bool tabsFit(int i = 0) { return i >= kTabCount || (kTabs[i].count <= kMaxItems && tabsFit(i + 1)); }
static_assert(tabsFit(), "a tab has more rows than the panel holds (kMaxItems)");

const Tr kTitle{"Ajustes de VR", "VR settings", "Réglages VR"};
const Tr kCloseText{"Cerrar", "Close", "Fermer"};
const Tr kYes{"Sí", "Yes", "Oui"}, kNo{"No", "No", "Non"};
const Tr kApply{"Aplicar", "Apply", "Appliquer"}, kSearch{"Buscar", "Check", "Vérifier"};
// the update check's state (updateText)
const Tr kBusy{"Buscando...", "Checking...", "Recherche..."};
const Tr kLatest{"Tienes la última (", "You have the latest (", "Vous avez la dernière ("};
const Tr kPublished{"; publicada: ", "; published: ", " ; publiée : "};
const Tr kNewOne{"Hay una nueva: ", "A new one: ", "Nouvelle version : "};
const Tr kYouHave{" (tienes ", " (you have ", " (vous avez "};
const Tr kOffline{"Error: sin conexión", "Error: no connection", "Erreur : pas de connexion"};
const Tr kError{"Error: ", "Error: ", "Erreur : "};

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

// 1.0.5: wider (the six tabs' names) and larger in the room; the controls on the right as before.
// Fase 2: the tabs in two rows (nine of them), the rows below them
const int kTexW = 900;
const float kRight = kTexW - 640.0f;  // the controls' shift from the 640-wide panel's places
const float kTabsY0 = 92.0f, kTabH = 56.0f, kTabGap = 8.0f, kTabsX0 = 16.0f, kTabsX1 = kTexW - 16.0f;
const float kRowsTop = kTabsY0 + 2.0f * kTabH + kTabGap + 24.0f, kRowH = 104.0f;
const int kTexH = (int)(kRowsTop + kMaxItems * kRowH + 24.0f);
const float kWidthM = 0.8f;
const float kCloseX0 = 448.0f + kRight, kCloseX1 = 608.0f + kRight, kCloseY0 = 24.0f, kCloseY1 = 70.0f;
const float kControlY = 36.0f;
const float kButtonR = 28.0f;
const float kStepMinusX = 380.0f + kRight, kStepPlusX = 580.0f + kRight;
const float kSwitchX0 = 520.0f + kRight, kSwitchX1 = 608.0f + kRight, kSwitchR = 22.0f;

// A tab's button, in its row, as wide as its name plus an even share of the rest of the row
void tabSpan(int t, float* x0, float* x1, float* y0, float* y1) {
    const int row = t < kTabsFirstRow ? 0 : 1;
    const int first = row == 0 ? 0 : kTabsFirstRow, last = row == 0 ? kTabsFirstRow : kTabCount;
    float names = 0.0f;
    for (int i = first; i < last; i++) names += ui::textWidth(kFontSmall, tr(kTabs[i].name));
    const float spare = std::max(0.0f, (kTabsX1 - kTabsX0 - names) / float(last - first));
    float x = kTabsX0;
    for (int i = first; i < t; i++) x += ui::textWidth(kFontSmall, tr(kTabs[i].name)) + spare;
    *x0 = x;
    *x1 = x + ui::textWidth(kFontSmall, tr(kTabs[t].name)) + spare;
    *y0 = kTabsY0 + row * (kTabH + kTabGap);
    *y1 = *y0 + kTabH;
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
    int staticsLang = -1;  // the language the statics were drawn in
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

// a number's decimal point as the language writes it (a comma in Spanish and French)
std::string decimals(std::string s) {
    if (sLang == 1) return s;
    for (char& c : s)
        if (c == '.') c = ',';
    return s;
}

// The update check's state (the Host's info("update"): "busy", "latest", "new" or "error", its versions
// or message after tabs) as the panel's hint; "" while there is none yet.
std::string updateText() {
    const std::string st = p.host.info ? p.host.info("update") : std::string();
    if (st.empty()) return std::string();
    std::vector<std::string> f;
    size_t at = 0;
    for (;;) {
        const size_t tab = st.find('\t', at);
        f.push_back(st.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
        if (tab == std::string::npos) break;
        at = tab + 1;
    }
    auto field = [&](size_t i) { return i < f.size() ? f[i] : std::string("?"); };
    if (f[0] == "busy") return tr(kBusy);
    if (f[0] == "latest") {
        // latest, local, published (older than the local one, say a build not published yet)
        std::string t = std::string(tr(kLatest)) + field(1);
        if (f.size() > 2 && field(2) != field(1)) t += std::string(tr(kPublished)) + field(2);
        return t + ")";
    }
    if (f[0] == "new")  // new, published, local
        return std::string(tr(kNewOne)) + field(1) + tr(kYouHave) + field(2) + ")";
    if (f[0] == "error") {
        const std::string why = field(1);
        if (why == "offline") return tr(kOffline);
        return std::string(tr(kError)) + why;
    }
    return st;
}

// A row's hint: its own, or the update check's state
std::string hintText(const Item& it) {
    if (it.src == kAction && !strcmp(it.key, "check_update")) {
        const std::string u = updateText();
        if (!u.empty()) return u;
    }
    return tr(it.hint);
}

std::string valueText(const Item& it) {
    if (it.kind == kInfo) return it.src == kAction && !strcmp(it.key, "version") && p.host.info ? p.host.info("version") : "";
    const std::string v = itemValue(it);
    if (it.kind == kSwitch) return tr(isOn(v) ? kYes : kNo);
    if (it.kind == kButton) return "";
    if (it.options) {
        // the option, or the nearest number's (a value set elsewhere, between two)
        for (int i = 0; i < it.optionCount; i++)
            if (v == it.options[i].value) return tr(it.options[i].text);
        return v.empty() ? "-" : decimals(v);  // (no dash in the font)
    }
    char t[48];
    snprintf(t, sizeof t, it.format ? it.format : "%g", atof(v.c_str()));
    return decimals(t);
}

void stepItem(const Item& it, int dir) {
    if (it.kind == kInfo || it.kind == kButton) return;
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
    if (it.kind == kInfo) return;
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
    } else if (!strcmp(it.key, "check_update")) {
        if (p.host.checkUpdate) p.host.checkUpdate();
        plog("checking for an update");
    }
    p.dirty = true;
}

float rowTop(int i) { return kRowsTop + kRowH * i; }

std::string shownState() {
    std::string st = std::to_string(sLang) + ":" + std::to_string(p.tab) + ":" + std::to_string(p.focus) + ":";
    const Tab& t = kTabs[p.tab];
    for (int i = 0; i < t.count; i++) {
        st += valueText(t.items[i]) + "|";
        if (t.items[i].kind == kButton && t.items[i].src == kAction && !strcmp(t.items[i].key, "check_update"))
            st += hintText(t.items[i]) + "|";  // the check's state, as it changes
    }
    return st;
}

void drawStatic() {
    Canvas& c = p.statics;
    c.init(kTexW, kTexH);
    c.roundRect(0, 0, kTexW, kTexH, 30.0f, rgb(78, 86, 106, 0.95f));
    c.roundRect(2, 2, kTexW - 2, kTexH - 2, 28.0f, rgb(22, 25, 33));
    c.text(32, 62, kFontLarge, tr(kTitle), rgb(255, 255, 255));
    p.staticsLang = sLang;
}

void drawPanel() {
    if (p.staticsLang != sLang) drawStatic();
    p.canvas.px = p.statics.px;
    Canvas& c = p.canvas;
    const Color accent = rgb(86, 160, 255);
    const Color bright = rgb(196, 202, 216), dim = rgb(140, 147, 163), white = rgb(255, 255, 255),
                line = rgb(58, 63, 78);
    auto fill = [&](const Hit& h) { return p.pressed == h ? accent : p.hover == h ? rgb(84, 93, 116) : rgb(44, 49, 62); };
    const Tab& tab = kTabs[p.tab];

    c.roundRect(kCloseX0, kCloseY0, kCloseX1, kCloseY1, 23.0f, fill({kClose, 0}));
    c.text((kCloseX0 + kCloseX1) * 0.5f, 56, kFontSmall, tr(kCloseText), rgb(230, 234, 242), 1);
    for (int t = 0; t < kTabCount; t++) {
        float x0, x1, y0, y1;
        tabSpan(t, &x0, &x1, &y0, &y1);
        x0 += 3.0f, x1 -= 3.0f;
        const Hit h{kTabButton, t};
        const Color back = t == p.tab ? accent : p.pressed == h || p.hover == h ? rgb(84, 93, 116) : rgb(44, 49, 62);
        c.roundRect(x0, y0, x1, y1, 20.0f, back);
        c.text((x0 + x1) * 0.5f, y0 + 38.0f, kFontSmall, tr(kTabs[t].name), t == p.tab ? white : bright, 1);
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
        c.text(32, top + 46.0f, kFontSmall, tr(it.label), bright);
        c.text(32, top + 84.0f, kFontSmall, hintText(it).c_str(), dim);
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
        } else if (it.kind == kButton) {
            const Hit h{kPress, i};
            const bool search = it.src == kAction && !strcmp(it.key, "check_update");
            c.roundRect(kStepMinusX - kButtonR, y - 24, kStepPlusX + kButtonR, y + 24, 24.0f, fill(h));
            c.text((kStepMinusX + kStepPlusX) * 0.5f, y + 10, kFontSmall, tr(search ? kSearch : kApply), white, 1);
        } else if (!text.empty()) {
            // an information row's value (the version), where a stepper's would be
            c.text(kStepPlusX + kButtonR, y + 10, kFontSmall, text.c_str(), white, 2);
        }
    }
    p.shown = shownState();
}

Hit hitAt(float x, float y) {
    auto near = [](float x, float y, float cx, float cy, float r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
    if (x >= kCloseX0 - 8 && x <= kCloseX1 + 8 && y >= kCloseY0 - 8 && y <= kCloseY1 + 8) return {kClose, 0};
    for (int t = 0; t < kTabCount; t++) {
        float x0, x1, y0, y1;
        tabSpan(t, &x0, &x1, &y0, &y1);
        if (x >= x0 && x < x1 && y >= y0 - kTabGap * 0.5f && y <= y1 + kTabGap * 0.5f) return {kTabButton, t};
    }
    const Tab& tab = kTabs[p.tab];
    for (int i = 0; i < tab.count; i++) {
        const Item& it = tab.items[i];
        const float top = rowTop(i), cy = top + kControlY;
        if (y < top + 2.0f || y > top + kRowH - 2.0f) continue;
        if (it.kind == kInfo) continue;
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
    sLang = languageOf(p.host.xrGet("language"));  // xr.ini's, as it is now
    if (!p.dirty && shownState() != p.shown) p.dirty = true;  // a value changed elsewhere (the game's, the language)
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
