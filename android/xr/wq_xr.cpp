// WakerQuest's headset layer: the game on a giant screen in the Meta Quest (OpenXR), and the Touch
// controllers as a GameCube controller.
//
// Modelled on Sunshine Quest's gx_xr.cpp and pad.cpp (the Khronos loader started with
// xrInitializeLoaderKHR, XR_KHR_android_create_instance and XR_KHR_opengl_es_enable, an sRGB
// swapchain for the screen, the same Touch bindings). The difference is the renderer: Aurora draws
// with Dawn on Vulkan on its own threads, so this layer runs apart from it, on a thread of its own
// with its own EGL context, at the headset's rate:
//   - Aurora's present pass draws each frame into one of its three AHardwareBuffers (Aurora patch
//     0023, include/aurora/xr.h) and publishes it with a sync file. Each XR frame takes the newest,
//     waits for that fence on the GPU (EGL_ANDROID_native_fence_sync), samples the buffer through an
//     EGLImage and copies it to the screen's swapchain; when the copy has run it hands the buffer
//     back (aurora_xr_present_done).
//   - The screen is a quad layer in the LOCAL space, 4:3, by default 4.5 m wide and 5 m away, over the
//     room (XR_FB_passthrough) when the headset has it.
//   - The controllers drive an SDL virtual gamepad, which Aurora sees as any other controller: left
//     stick = control stick, right stick = C-stick, A/B/X/Y as labelled, triggers = L and R (analog),
//     right grip = Z, the left controller's menu button = START. Aurora's rumble reaches both hands.
// Settings: <files>/user/xr.ini (distance, width, height, passthrough, refresh), read again when it
// changes (checked every second). Every step goes to switchwaker.log as "[xr] ...".
#include "wq_xr.h"
#include "wq_panel.h"

#include <aurora/xr.h>

extern "C" __attribute__((noreturn)) void pc_exit(int code);  // native/include/pc/pc_harness.h

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h> // after gl3.h, which defines its types
#include <SDL3/SDL.h>
#include <android/hardware_buffer.h>
#include <jni.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace {

void xrlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void xrlog(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[xr] %s\n", line);
    fflush(stderr);
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- Settings (<files>/user/xr.ini) --------------------------------------------------------------

struct Settings {
    float distance = 5.0f;   // m in front of where the player looked at the start
    float width = 4.5f;      // m; the height follows the picture (4:3)
    float height = 0.0f;     // m above eye height
    bool passthrough = true; // the room around the screen instead of black
    int refresh = 90;        // Hz, the headset's nearest at or below; 90 shows the game's 30 fps evenly
    bool gpuBoost = true;    // XR_EXT_performance_settings: the GPU at its sustained high level
    // stereoscopic 3D (Aurora patch 0024): each eye its own picture of the scene
    bool stereo = true;
    float stereoStrength = 0.012f;  // how far the distance moves per eye, in half picture widths
    float stereoFocus = 600.0f;     // the depth (game units) that stays on the screen; nearer comes out of it
    bool cine3d = true;             // cutscenes (the game's events): more depth and more coming out
    float hudWidth = 1.3f;          // the diorama's HUD panel, metres across (behind the model)
    // the game's internal height in pixels in the headset (COS_FB_SCALE = resolution / 480), read at
    // start; 0 = the headset window's own (4128x2208, which drawn twice per frame is too much)
    int resolution = 1512;  // 1.40 of 1080 lines (the menu's scale): the game holds 30 fps with it
    // the diorama (mode = maqueta, pc_vr_diorama.cpp): the world as a model in the room, in a bubble
    // (a sphere, as GalaxyQuest's) or on a table (a box)
    bool diorama = false;
    bool sphere = true;
    float tableWidth = 1.0f, tableDepth = 0.7f;  // m: the bubble's diameter, or the table's width and depth
    float tableSpan = 30.0f;                     // m of the game's world across it (more: more to draw)
    float tableDistance = 1.0f;                  // m, its middle in front of the eyes
    float tableHeight = 0.3f;                    // m, Link (the table's top) below the eyes
    float tableFade = 0.1f;                      // m, the edges' fade
    bool tableFollowYaw = true;                  // the world turns with the game camera
    bool viewRelative = true;                    // the left stick relative to the player's view of the model
    bool autoScreen = true;                      // first-person views on the giant screen
    bool windowHudOn = true;                     // the window: the HUD and dialogues over it (else on panels)
    bool cineScreen = true;                      // and cutscenes
    bool birdsFree = true;                       // the flying birds around the room, outside the bubble
    float birdRange = 3.0f;                      // how far, in the bubble's radii
    float tableX = 0.0f;                         // m, to the right of straight ahead (the grip moves it)
    float tableYaw = 0.0f;                       // degrees, the world turned by the grip
    float dialogueBack = 0.3f;                   // m, the dialogue panel behind the model's middle
    // the HUD's panel, fixed in the room (m in the LOCAL space, and its yaw in degrees): moved only by
    // a grip on it, not with the model
    float hudPose[4] = {-0.03f, -0.22f, -1.39f, 0.0f};
    // m, the dialogues' panel moved by hand (a grip on it) from its place above the model; it moves with it
    float dialogueOffset[3] = {0, 0, 0};
    // the window (mode = ventana, pc_vr_diorama.cpp): the world at its own scale through a rectangle in
    // the room (16:9), the game camera's eye window_depth in front of it looking through it (what it sees
    // nearer than that comes out, up to window_popout); each eye looks through it from
    // where it is, and what is near comes out of it toward the player (window_popout)
    bool window = false;
    float windowWidth = 1.6f;     // m across
    float windowDistance = 2.0f;  // m, its middle in front of the eyes
    float windowHeight = 0.0f;    // m above eye height
    float windowX = 0.0f;         // m to the right (a grip moves it)
    float windowYaw = 0.0f;       // degrees, turned to the left (a grip turns it)
    float windowScale = 1.0f;     // the game's metres per metre of the room (more: the world smaller)
    float windowDepth = 0.5f;     // m, the game camera's eye in front of the window (caps the pop-out)
    bool windowPopoutOn = true;   // near things come out of the window
    float windowPopout = 0.45f;    // m, how far
    float windowFade = 0.03f;     // m, the edges' fade
    bool windowVignette = false;  // a comfort vignette when the camera moves fast (not done yet)
};

const char kDefaultSettings[] =
    "# WakerQuest: the screen in the headset. Read again while playing.\n"
    "# distance and width in metres, height in metres above the eyes,\n"
    "# passthrough 1 = the room around the screen, 0 = black; refresh in Hz (72, 80, 90, 120).\n"
    "distance = 5.0\n"
    "width = 4.5\n"
    "height = 0.0\n"
    "passthrough = 1\n"
    "refresh = 90\n"
    "# 3D: stereo 1/0; stereo_strength = depth of what is far (0.004-0.03),\n"
    "# stereo_focus = the distance in the game that stays on the screen (100-3000).\n"
    "stereo = 1\n"
    "stereo_strength = 0.012\n"
    "stereo_focus = 600\n"
    "# the game's internal height in pixels (720, 900, 1080, 1200, 1440); read when the game starts.\n"
    "resolution = 1512\n"
    "# mode = pantalla (a giant screen), maqueta (the world as a model in the room) or ventana (a window).\n"
    "mode = pantalla\n"
    "# the model: shape = esfera (a bubble, as in Galaxy) or caja (a table); bubble_size = its diameter\n"
    "# in m (the table's width), bubble_span = metres of the game across it, bubble_distance and\n"
    "# bubble_height (Link below the eyes) in m, bubble_fade = the edge's fade in m; table_depth (cm) for\n"
    "# the box; table_follow_yaw 1 = the world turns with the game camera. Also in the options menu.\n"
    "shape = esfera\n"
    "bubble_size = 1.0\n"
    "bubble_span = 30\n"
    "bubble_distance = 1.0\n"
    "bubble_height = 0.3\n"
    "bubble_fade = 0.1\n"
    "table_depth = 70\n"
    "table_follow_yaw = 1\n"
    "# mode = ventana: the world at its own size through a window in the room. window_width (m, 16:9),\n"
    "# window_distance, window_height (above the eyes), window_x in m; window_yaw in degrees;\n"
    "# window_scale = metres of the game per metre (1 = real size); window_depth = the game camera in front\n"
    "# of the window (m); window_popout_on 1 = near things come out of it, up to window_popout m (and\n"
    "# never past the camera).\n"
    "window_width = 1.6\n"
    "window_distance = 2.0\n"
    "window_height = 0.0\n"
    "window_scale = 1.0\n"
    "window_depth = 0.5\n"
    "window_popout_on = 1\n"
    "window_popout = 0.45\n";

void applySetting(Settings& s, const char* key, const char* v) {
    const float f = (float)atof(v);
    const bool on = !(!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "false"));
    if (!strcmp(key, "distance")) s.distance = clampf(f, 0.5f, 30.0f);
    else if (!strcmp(key, "width")) s.width = clampf(f, 0.5f, 40.0f);
    else if (!strcmp(key, "height")) s.height = clampf(f, -10.0f, 10.0f);
    else if (!strcmp(key, "passthrough")) s.passthrough = on;
    else if (!strcmp(key, "refresh")) s.refresh = int(f);
    else if (!strcmp(key, "stereo")) s.stereo = on;
    else if (!strcmp(key, "stereo_strength")) s.stereoStrength = clampf(f, 0.0f, 0.10f);
    else if (!strcmp(key, "stereo_focus")) s.stereoFocus = clampf(f, 1.0f, 2500.0f);
    else if (!strcmp(key, "cine_3d")) s.cine3d = on;
    else if (!strcmp(key, "resolution")) s.resolution = f <= 0.0f ? 0 : int(clampf(f, 480.0f, 2400.0f));
    else if (!strcmp(key, "mode")) {
        s.diorama = !strcmp(v, "maqueta") || !strcmp(v, "diorama") || !strcmp(v, "table");
        s.window = !strcmp(v, "ventana") || !strcmp(v, "window");
    }
    else if (!strcmp(key, "shape")) s.sphere = strcmp(v, "caja") != 0 && strcmp(v, "box") != 0 && strcmp(v, "table") != 0;
    else if (!strcmp(key, "bubble_size")) s.tableWidth = clampf(f, 0.2f, 4.0f);
    else if (!strcmp(key, "bubble_span") || !strcmp(key, "table_span")) s.tableSpan = clampf(f, 2.0f, 500.0f);
    else if (!strcmp(key, "bubble_distance") || !strcmp(key, "table_distance")) s.tableDistance = clampf(f, 0.2f, 5.0f);
    else if (!strcmp(key, "bubble_height") || !strcmp(key, "table_height")) s.tableHeight = clampf(f, -1.0f, 2.0f);
    else if (!strcmp(key, "bubble_fade") || !strcmp(key, "table_fade")) s.tableFade = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "view_relative")) s.viewRelative = on;
    else if (!strcmp(key, "bubble_x")) s.tableX = clampf(f, -5.0f, 5.0f);
    else if (!strcmp(key, "bubble_yaw")) s.tableYaw = f;
    else if (!strcmp(key, "table_width")) s.tableWidth = clampf(f, 20.0f, 400.0f) / 100.0f;
    else if (!strcmp(key, "table_depth")) s.tableDepth = clampf(f, 20.0f, 400.0f) / 100.0f;
    else if (!strcmp(key, "table_follow_yaw")) s.tableFollowYaw = on;
    else if (!strcmp(key, "hud_width")) s.hudWidth = clampf(f, 0.2f, 3.0f);
    else if (!strcmp(key, "gpu_boost")) s.gpuBoost = on;
    else if (!strcmp(key, "auto_screen")) s.autoScreen = on;
    else if (!strcmp(key, "window_hud_on")) s.windowHudOn = on;
    else if (!strcmp(key, "cine_screen")) s.cineScreen = on;
    else if (!strcmp(key, "birds_free")) s.birdsFree = on;
    else if (!strcmp(key, "bird_range")) s.birdRange = clampf(f, 1.0f, 8.0f);
    else if (!strcmp(key, "dialogue_back")) s.dialogueBack = clampf(f, -1.0f, 3.0f);
    else if (!strcmp(key, "window_width")) s.windowWidth = clampf(f, 0.3f, 6.0f);
    else if (!strcmp(key, "window_distance")) s.windowDistance = clampf(f, 0.3f, 10.0f);
    else if (!strcmp(key, "window_height")) s.windowHeight = clampf(f, -3.0f, 3.0f);
    else if (!strcmp(key, "window_x")) s.windowX = clampf(f, -5.0f, 5.0f);
    else if (!strcmp(key, "window_yaw")) s.windowYaw = std::remainder(f, 360.0f);  // -180..180
    else if (!strcmp(key, "window_scale")) s.windowScale = clampf(f, 0.1f, 100.0f);
    else if (!strcmp(key, "window_depth")) s.windowDepth = clampf(f, 0.05f, 5.0f);
    else if (!strcmp(key, "window_popout_on")) s.windowPopoutOn = on;
    else if (!strcmp(key, "window_popout")) s.windowPopout = clampf(f, 0.0f, 3.0f);
    else if (!strcmp(key, "window_fade")) s.windowFade = clampf(f, 0.0f, 0.5f);
    else if (!strcmp(key, "window_vignette")) s.windowVignette = on;
    else if (!strcmp(key, "hud_pose")) {
        float x = 0, y = 0, z = 0, yaw = 0;
        if (sscanf(v, "%f,%f,%f,%f", &x, &y, &z, &yaw) == 4) {
            s.hudPose[0] = clampf(x, -10.0f, 10.0f);
            s.hudPose[1] = clampf(y, -10.0f, 10.0f);
            s.hudPose[2] = clampf(z, -10.0f, 10.0f);
            s.hudPose[3] = yaw;
        }
    } else if (!strcmp(key, "dialogue_offset")) {
        float x = 0, y = 0, z = 0;
        if (sscanf(v, "%f,%f,%f", &x, &y, &z) == 3) {
            s.dialogueOffset[0] = clampf(x, -5.0f, 5.0f);
            s.dialogueOffset[1] = clampf(y, -5.0f, 5.0f);
            s.dialogueOffset[2] = clampf(z, -5.0f, 5.0f);
        }
    }
}

// The value in effect of an xr.ini key (the settings panel's), as the file would hold it.
std::string settingValue(const Settings& s, const char* key) {
    char b[48];
    auto num = [&](float v) {
        snprintf(b, sizeof b, "%g", v);
        return std::string(b);
    };
    auto flag = [](bool v) { return std::string(v ? "1" : "0"); };
    if (!strcmp(key, "mode")) return s.window ? "ventana" : s.diorama ? "maqueta" : "pantalla";
    if (!strcmp(key, "shape")) return s.sphere ? "esfera" : "caja";
    if (!strcmp(key, "refresh")) return num(float(s.refresh));
    if (!strcmp(key, "gpu_boost")) return flag(s.gpuBoost);
    if (!strcmp(key, "auto_screen")) return flag(s.autoScreen);
    if (!strcmp(key, "window_hud_on")) return flag(s.windowHudOn);
    if (!strcmp(key, "cine_screen")) return flag(s.cineScreen);
    if (!strcmp(key, "birds_free")) return flag(s.birdsFree);
    if (!strcmp(key, "bird_range")) return num(s.birdRange);
    if (!strcmp(key, "stereo_strength")) return num(s.stereoStrength);
    if (!strcmp(key, "stereo_focus")) return num(s.stereoFocus);
    if (!strcmp(key, "cine_3d")) return flag(s.cine3d);
    if (!strcmp(key, "passthrough")) return flag(s.passthrough);
    if (!strcmp(key, "stereo")) return flag(s.stereo);
    if (!strcmp(key, "distance")) return num(s.distance);
    if (!strcmp(key, "width")) return num(s.width);
    if (!strcmp(key, "bubble_size")) return num(s.tableWidth);
    if (!strcmp(key, "bubble_span")) return num(s.tableSpan);
    if (!strcmp(key, "bubble_distance")) return num(s.tableDistance);
    if (!strcmp(key, "bubble_height")) return num(s.tableHeight);
    if (!strcmp(key, "bubble_fade")) return num(s.tableFade);
    if (!strcmp(key, "hud_width")) return num(s.hudWidth);
    if (!strcmp(key, "dialogue_back")) return num(s.dialogueBack);
    if (!strcmp(key, "table_follow_yaw")) return flag(s.tableFollowYaw);
    if (!strcmp(key, "view_relative")) return flag(s.viewRelative);
    if (!strcmp(key, "window_width")) return num(s.windowWidth);
    if (!strcmp(key, "window_distance")) return num(s.windowDistance);
    if (!strcmp(key, "window_height")) return num(s.windowHeight);
    if (!strcmp(key, "window_scale")) return num(s.windowScale);
    if (!strcmp(key, "window_depth")) return num(s.windowDepth);
    if (!strcmp(key, "window_popout_on")) return flag(s.windowPopoutOn);
    if (!strcmp(key, "window_popout")) return num(s.windowPopout);
    if (!strcmp(key, "window_fade")) return num(s.windowFade);
    if (!strcmp(key, "window_vignette")) return flag(s.windowVignette);
    return std::string();
}

// ---- State ---------------------------------------------------------------------------------------

struct Cached {  // an Aurora buffer as a GL texture
    AHardwareBuffer* buffer = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
};

struct Xr {
    JavaVM* vm = nullptr;
    jobject activity = nullptr;  // global reference, kept for the run
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    bool hasRefreshRate = false, hasPassthrough = false;

    // EGL: a context of our own on a 16x16 pbuffer
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC nativeClientBuffer = nullptr;
    PFNEGLCREATESYNCKHRPROC createSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
    PFNEGLWAITSYNCKHRPROC waitSync = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC clientWaitSync = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture = nullptr;

    XrSession session = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, focused = false, exiting = false;
    XrSpace space = XR_NULL_HANDLE;
    XrFrameState frame{XR_TYPE_FRAME_STATE};

    // the screen
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;
    uint32_t screenW = 0, screenH = 0;
    bool srgb = false, haveScreen = false;
    bool sideBySide = false, stereoFrame = false;  // the swapchain's picture: two eyes wide, both eyes in it
    GLuint copyProg = 0;
    GLint copyULinear = -1;
    std::vector<Cached> cache;
    EGLSyncKHR pendingRead = EGL_NO_SYNC_KHR;  // the copy from the last taken buffer
    uint32_t frames = 0;
    uint32_t framesWithoutFence = 0;  // Aurora published them without a sync file: read unsynchronized
    uint32_t lastSerial = 0, skipped = 0;  // Aurora frames never taken (replaced by a newer one first)

    // passthrough
    bool passthroughRunning = false;
    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;

    // controllers
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction move = XR_NULL_HANDLE, camera = XR_NULL_HANDLE;
    XrAction a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, x = XR_NULL_HANDLE, y = XR_NULL_HANDLE;
    XrAction z = XR_NULL_HANDLE, start = XR_NULL_HANDLE;
    XrAction trigL = XR_NULL_HANDLE, trigR = XR_NULL_HANDLE;
    XrAction rumble = XR_NULL_HANDLE;
    XrAction handPose = XR_NULL_HANDLE, stickClick = XR_NULL_HANDLE;  // the diorama's grip drag
    XrSpace handSpace = XR_NULL_HANDLE;
    // the options menu's laser pointer
    XrAction aimPose = XR_NULL_HANDLE;
    XrSpace aimSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    float trig[2] = {0.0f, 0.0f};      // the triggers as read this frame
    bool trigDown[2] = {false, false};
    int activeHand = 1;                // the hand that last pulled its trigger
    bool menuWasOpen = false;
    XrPosef menuPose{};                // the menu's panel, placed when it opens (the diorama)
    bool clickDown = false;
    bool hoverWas = false;
    float mouseX = -1.0f, mouseY = -1.0f;
    XrSwapchain cursorSwapchain = XR_NULL_HANDLE, raySwapchain = XR_NULL_HANDLE;
    // the grips: the model moved by one hand, moved, turned and scaled by both, or a panel (the HUD's,
    // the dialogues') moved by the hand whose ray or grip is on it
    XrAction gripL = XR_NULL_HANDLE, handPoseL = XR_NULL_HANDLE;
    XrSpace handSpaceL = XR_NULL_HANDLE;
    bool gripDown[2] = {false, false};
    int grabbed[2] = {0, 0};  // the panel a hand holds: 1 the HUD, 2 the dialogues
    XrVector3f grabHand[2]{};
    float grabOffset[2][3]{};
    int dragHands = 0;        // the hands on the model when its drag (re)started: 1 left, 2 right, 3 both
    bool dragging = false;
    XrVector3f dragHand{};
    float dragHandYaw = 0.0f;
    float dragX = 0.0f, dragHeight = 0.0f, dragDistance = 0.0f, dragYaw = 0.0f;
    float dragSpan = 0.0f, dragSize = 0.0f, tickSize = 0.0f;  // both hands: their distance, the size then
    uint64_t sizeShownUntil = 0;                             // the size's label (ms, SDL_GetTicks)
    bool clickWas = false;
    // the panels where they are shown this frame (for the grips and the rays)
    bool hudShown = false, dialogueShown = false;
    XrPosef hudPanel{}, dialoguePanel{};
    XrExtent2Df hudSize{}, dialogueSize{};
    // the settings panel
    bool panelReady = false;
    bool comboWas = false, comboHeld = false;  // both triggers + the right grip
    bool hasPerf = false;
    int perfApplied = -1;
    XrPath hands[2] = {XR_NULL_PATH, XR_NULL_PATH};
    SDL_JoystickID padId = 0;
    SDL_Joystick* pad = nullptr;
    int rumbleNow = 0;

    Settings settings;
    char settingsPath[600] = "";
    time_t settingsTime = 0;
    time_t settingsChecked = 0;

    // the diorama: the swapchain's picture was drawn for head pose `frameTag`
    bool dioramaFrame = false;
    uint32_t frameTag = 0;
    bool hudFrame = false;  // the buffer's third part is the HUD's panel (Aurora patch 0026)
    GLint copyUAlpha = -1;
};
Xr s;

// Shared with the game thread (wq_xr_head, wq_xr_table): the settings and the head poses of the last
// frames, each named by a tag, so a diorama frame is shown with the pose it was drawn for.
struct HeadSnap {
    WqXrHead head{};
    XrPosef eyes[2]{};
};
std::mutex s_shared;
Settings s_sharedSettings;
bool s_sharedRunning = false;
constexpr int kHeadHistory = 64;
HeadSnap s_heads[kHeadHistory];
uint32_t s_nextTag = 1;

std::atomic_int s_rumbleWanted{0};  // set by SDL's thread (the virtual gamepad's Rumble)
std::atomic<float> s_camHeading{0.0f};  // wq_xr_set_camera_heading (the game thread)
std::atomic_bool s_camHeadingValid{false};
std::atomic_bool s_rotateStick{false};   // the player controls Link freely
std::atomic_bool s_dialogue{false};      // a message box is open
std::atomic_bool s_menuOpen{false};      // wq_xr_menu_state: the options menu
std::atomic<float> s_menuW{0.0f}, s_menuH{0.0f};
std::atomic_bool s_menuHovered{false};
float s_viewHeading = 0.0f;  // the player's view of the model, smoothed (the layer's thread)
bool s_viewHeadingValid = false;

bool ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (s.instance) xrResultToString(s.instance, r, name);
    xrlog("%s failed: %d %s", what, int(r), name);
    return false;
}

XrPath toPath(const char* p) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(s.instance, p, &path);
    return path;
}

bool hasExtension(const std::vector<XrExtensionProperties>& exts, const char* name) {
    for (const XrExtensionProperties& e : exts)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

std::atomic_bool s_settingsDirty{false};  // wq_xr_set wrote the file: read it now
std::atomic_bool s_cinematic{false};       // wq_xr_set_cinematic: the game runs an event (a cutscene)
float s_stereoApplied[3] = {-1.0f, 0.0f, 0.0f};  // the screen's 3D as last given to Aurora: on, strength, focus
float s_cineRamp = 0.0f;                   // 0..1, the cutscenes' 3D faded in and out
bool s_refreshDirty = false;               // the refresh rate changed while running

void readSettings(bool force) {
    const time_t now = time(nullptr);
    if (s_settingsDirty.exchange(false)) {
        force = true;
        s.settingsTime = 0;
    }
    if (!force && now == s.settingsChecked) return;
    s.settingsChecked = now;
    struct stat st;
    if (stat(s.settingsPath, &st) != 0) {
        if (!force) return;
        // a file to edit, with the defaults
        if (FILE* f = fopen(s.settingsPath, "w")) {
            fputs(kDefaultSettings, f);
            fclose(f);
            xrlog("settings: wrote the defaults to %s", s.settingsPath);
        }
        if (stat(s.settingsPath, &st) != 0) return;
    }
    if (!force && st.st_mtime == s.settingsTime) return;
    s.settingsTime = st.st_mtime;
    FILE* f = fopen(s.settingsPath, "r");
    if (!f) return;
    Settings next;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char* key = line;
        char* v = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char* c = eq - 1; c >= key && (*c == ' ' || *c == '\t'); c--) *c = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char* c = v + strlen(v) - 1; c >= v && (*c == '\n' || *c == '\r' || *c == ' ' || *c == '\t'); c--)
            *c = 0;
        applySetting(next, key, v);
    }
    fclose(f);
    const bool refreshChanged = next.refresh != s.settings.refresh;
    s.settings = next;
    {
        std::lock_guard lock{s_shared};
        s_sharedSettings = next;
    }
    if (refreshChanged && !force) s_refreshDirty = true;  // the menu's Frecuencia, applied now
    // the screen's 3D; the diorama sets its own eye shift every frame (pc_vr_diorama.cpp)
    s_stereoApplied[0] = -1.0f;  // the screen's 3D applied again (updateScreenStereo)
    xrlog("settings: mode %s; table %.0fx%.0f cm, %.0f m of the game across, %.2f m away, %.2f m below the eyes, "
          "fade %.2f m, %s the camera",
          next.window ? "ventana" : next.diorama ? "maqueta" : "pantalla", next.tableWidth * 100.0f, next.tableDepth * 100.0f, next.tableSpan,
          next.tableDistance, next.tableHeight, next.tableFade, next.tableFollowYaw ? "turning with" : "not turning with");
    xrlog("settings: screen %.1f m wide, %.1f m away, %.1f m up; passthrough %s; refresh %d Hz; 3D %s "
          "(strength %.3f, focus %.0f)",
          next.width, next.distance, next.height, next.passthrough ? "on" : "off", next.refresh,
          next.stereo ? "on" : "off", next.stereoStrength, next.stereoFocus);
    xrlog("settings: window %.2f m wide, %.2f m away, %.2f m up, %.2f m right, %.0f deg; scale %.2f, camera %.2f m "
          "behind, coming out %s (%.2f m)",
          next.windowWidth, next.windowDistance, next.windowHeight, next.windowX, next.windowYaw, next.windowScale,
          next.windowDepth, next.windowPopoutOn ? "on" : "off", next.windowPopout);
}

// ---- Instance (on the game's thread, before Aurora) ----------------------------------------------

bool initInstance() {
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    if (!ok(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initLoader),
            "xrGetInstanceProcAddr(xrInitializeLoaderKHR)") ||
        !initLoader)
        return false;
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (!env || !activity) {
        xrlog("no Android activity");
        return false;
    }
    env->GetJavaVM(&s.vm);
    s.activity = env->NewGlobalRef(activity);
    env->DeleteLocalRef(activity);

    XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInfo.applicationVM = s.vm;
    loaderInfo.applicationContext = s.activity;
    if (!ok(initLoader((const XrLoaderInitInfoBaseHeaderKHR*)&loaderInfo), "xrInitializeLoaderKHR")) return false;

    uint32_t count = 0;
    if (!ok(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
            "xrEnumerateInstanceExtensionProperties"))
        return false;
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
    if (!hasExtension(exts, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
        xrlog("the runtime has no OpenGL ES support");
        return false;
    }
    std::vector<const char*> enable = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                                       XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
    s.hasRefreshRate = hasExtension(exts, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (s.hasRefreshRate) enable.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    s.hasPassthrough = hasExtension(exts, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (s.hasPassthrough) enable.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    s.hasPerf = hasExtension(exts, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (s.hasPerf) enable.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android.applicationVM = s.vm;
    android.applicationActivity = s.activity;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android;
    strcpy(ci.applicationInfo.applicationName, "WakerQuest");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "Aurora");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = uint32_t(enable.size());
    ci.enabledExtensionNames = enable.data();
    if (!ok(xrCreateInstance(&ci, &s.instance), "xrCreateInstance")) return false;

    XrSystemGetInfo sys{XR_TYPE_SYSTEM_GET_INFO};
    sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!ok(xrGetSystem(s.instance, &sys, &s.system), "xrGetSystem")) return false;
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(s.instance, s.system, &props);
    xrlog("instance: %s (refresh rate %s, passthrough %s)", props.systemName, s.hasRefreshRate ? "yes" : "no",
          s.hasPassthrough ? "yes" : "no");
    return true;
}

// ---- EGL and GL (on the layer's thread) ----------------------------------------------------------

template <typename T>
bool proc(T& out, const char* name) {
    out = reinterpret_cast<T>(eglGetProcAddress(name));
    if (!out) xrlog("EGL/GL: no %s", name);
    return out != nullptr;
}

bool initEgl() {
    s.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (s.display == EGL_NO_DISPLAY || !eglInitialize(s.display, nullptr, nullptr)) {
        xrlog("EGL: no display");
        return false;
    }
    const EGLint configAttr[] = {EGL_RED_SIZE,     8, EGL_GREEN_SIZE,      8,
                                 EGL_BLUE_SIZE,    8, EGL_ALPHA_SIZE,      8,
                                 EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                 EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(s.display, configAttr, &s.config, 1, &n) || n < 1) {
        xrlog("EGL: no RGBA8 ES 3 config");
        return false;
    }
    const EGLint contextAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    s.context = eglCreateContext(s.display, s.config, EGL_NO_CONTEXT, contextAttr);
    const EGLint pbufferAttr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    s.pbuffer = eglCreatePbufferSurface(s.display, s.config, pbufferAttr);
    if (s.context == EGL_NO_CONTEXT || s.pbuffer == EGL_NO_SURFACE ||
        !eglMakeCurrent(s.display, s.pbuffer, s.pbuffer, s.context)) {
        xrlog("EGL: context failed (0x%x)", eglGetError());
        return false;
    }
    if (!proc(s.createImage, "eglCreateImageKHR") || !proc(s.destroyImage, "eglDestroyImageKHR") ||
        !proc(s.nativeClientBuffer, "eglGetNativeClientBufferANDROID") ||
        !proc(s.createSync, "eglCreateSyncKHR") || !proc(s.destroySync, "eglDestroySyncKHR") ||
        !proc(s.waitSync, "eglWaitSyncKHR") || !proc(s.clientWaitSync, "eglClientWaitSyncKHR") ||
        !proc(s.imageTargetTexture, "glEGLImageTargetTexture2DOES"))
        return false;
    xrlog("EGL: GLES context ready (%s)", (const char*)glGetString(GL_RENDERER));
    return true;
}

GLuint compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint okc = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &okc);
    if (!okc) {
        char log[1024] = "";
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        xrlog("copy shader: %s", log);
    }
    return sh;
}

// The copy into the screen's swapchain. Aurora's buffer holds the game's sRGB-encoded colours with
// row 0 at the top; an sRGB swapchain encodes what is written, so they are decoded first, and GL's
// row 0 is the bottom of the image, so the picture is flipped.
bool initCopy() {
    static const char* vs = R"(#version 300 es
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
    static const char* fs = R"(#version 300 es
precision highp float;
uniform sampler2D u_tex;
uniform int u_linear;
uniform int u_alpha;  // the diorama: keep the alpha (straight in the buffer), premultiplied for OpenXR
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec4 t = texture(u_tex, v_uv);
    vec3 c = t.rgb;
    if (u_linear != 0)
        c = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
    float a = t.a;
    // the HUD's third: over its transparent background, the game's 2D often leaves the alpha unwritten
    // (the dialogue box, its text): what has colour shows
    if (u_alpha != 0 && v_uv.x > 0.6667) {
        // ... and the game paints a full-screen opaque black behind its 2D: pure black is the background
        float m = max(max(t.r, t.g), t.b);
        a = m < 0.02 ? 0.0 : max(a, smoothstep(0.02, 0.12, m));
    }
    o_color = u_alpha != 0 ? vec4(c * a, a) : vec4(c, 1.0);
}
)";
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    s.copyProg = glCreateProgram();
    glAttachShader(s.copyProg, v);
    glAttachShader(s.copyProg, f);
    glLinkProgram(s.copyProg);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint linked = 0;
    glGetProgramiv(s.copyProg, GL_LINK_STATUS, &linked);
    if (!linked) {
        xrlog("copy program did not link");
        return false;
    }
    glUseProgram(s.copyProg);
    glUniform1i(glGetUniformLocation(s.copyProg, "u_tex"), 0);
    s.copyULinear = glGetUniformLocation(s.copyProg, "u_linear");
    s.copyUAlpha = glGetUniformLocation(s.copyProg, "u_alpha");
    return true;
}

// An Aurora buffer as a texture, imported once (Aurora's ring has three; a size change makes three
// more, and the oldest beyond six are dropped). Each entry holds a reference to its buffer.
GLuint textureFor(AHardwareBuffer* buffer) {
    for (const Cached& c : s.cache)
        if (c.buffer == buffer) return c.texture;
    Cached c;
    c.buffer = buffer;
    AHardwareBuffer_acquire(buffer);
    EGLClientBuffer client = s.nativeClientBuffer(buffer);
    const EGLint attr[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    c.image = client ? s.createImage(s.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attr)
                     : EGL_NO_IMAGE_KHR;
    if (c.image == EGL_NO_IMAGE_KHR) {
        xrlog("eglCreateImageKHR for an Aurora buffer failed (0x%x)", eglGetError());
        AHardwareBuffer_release(buffer);
        return 0;
    }
    glGenTextures(1, &c.texture);
    glBindTexture(GL_TEXTURE_2D, c.texture);
    s.imageTargetTexture(GL_TEXTURE_2D, (GLeglImageOES)c.image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    while (s.cache.size() >= 6) {
        Cached& old = s.cache.front();
        glDeleteTextures(1, &old.texture);
        s.destroyImage(s.display, old.image);
        AHardwareBuffer_release(old.buffer);
        s.cache.erase(s.cache.begin());
    }
    s.cache.push_back(c);
    xrlog("imported Aurora buffer %p as texture %u", (void*)buffer, c.texture);
    return c.texture;
}

// ---- Session -------------------------------------------------------------------------------------

bool initSession() {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(s.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReqs);
    if (!getReqs) return false;
    XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!ok(getReqs(s.instance, s.system, &reqs), "xrGetOpenGLESGraphicsRequirementsKHR")) return false;

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = s.display;
    binding.config = s.config;
    binding.context = s.context;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = s.system;
    if (!ok(xrCreateSession(s.instance, &sci, &s.session), "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    if (!ok(xrCreateReferenceSpace(s.session, &rs, &s.space), "xrCreateReferenceSpace")) return false;

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(s.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(s.session, fmtCount, &fmtCount, fmts.data());
    for (int64_t f : fmts) s.srgb |= f == GL_SRGB8_ALPHA8;
    xrlog("session created (swapchain %s)", s.srgb ? "sRGB" : "RGBA8");
    return true;
}

// The screen's swapchain, the size of Aurora's buffers.
bool ensureSwapchain(uint32_t w, uint32_t h) {
    if (s.swapchain && w == s.screenW && h == s.screenH) return true;
    if (s.swapchain) {
        glDeleteFramebuffers(GLsizei(s.fbos.size()), s.fbos.data());
        xrDestroySwapchain(s.swapchain);
        s.swapchain = XR_NULL_HANDLE;
        s.haveScreen = false;
    }
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sc.format = s.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    sc.sampleCount = 1;
    sc.width = w;
    sc.height = h;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    if (!ok(xrCreateSwapchain(s.session, &sc, &s.swapchain), "xrCreateSwapchain")) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(s.swapchain, 0, &n, nullptr);
    s.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(s.swapchain, n, &n, (XrSwapchainImageBaseHeader*)s.images.data());
    s.fbos.assign(n, 0);
    glGenFramebuffers(GLsizei(n), s.fbos.data());
    for (uint32_t i = 0; i < n; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.images[i].image, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s.screenW = w;
    s.screenH = h;
    xrlog("screen swapchain %ux%u, %u images", w, h, n);
    return true;
}

XrAction makeAction(const char* name, XrActionType type, bool bothHands) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    strcpy(ci.actionName, name);
    strcpy(ci.localizedActionName, name);
    ci.actionType = type;
    if (bothHands) {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = s.hands;
    }
    XrAction act = XR_NULL_HANDLE;
    ok(xrCreateAction(s.actionSet, &ci, &act), name);
    return act;
}

bool initActions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "gamecube");
    strcpy(si.localizedActionSetName, "GameCube controller");
    if (!ok(xrCreateActionSet(s.instance, &si, &s.actionSet), "xrCreateActionSet")) return false;
    s.hands[0] = toPath("/user/hand/left");
    s.hands[1] = toPath("/user/hand/right");
    s.move = makeAction("move", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    s.camera = makeAction("camera", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    s.a = makeAction("a", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s.b = makeAction("b", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s.x = makeAction("x", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s.y = makeAction("y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s.z = makeAction("z", XR_ACTION_TYPE_FLOAT_INPUT, false);
    s.start = makeAction("start", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s.trigL = makeAction("trigger_l", XR_ACTION_TYPE_FLOAT_INPUT, false);
    s.trigR = makeAction("trigger_r", XR_ACTION_TYPE_FLOAT_INPUT, false);
    s.rumble = makeAction("rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
    s.handPose = makeAction("hand_pose", XR_ACTION_TYPE_POSE_INPUT, false);
    s.aimPose = makeAction("aim_pose", XR_ACTION_TYPE_POSE_INPUT, true);
    s.stickClick = makeAction("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s.gripL = makeAction("grip_l", XR_ACTION_TYPE_FLOAT_INPUT, false);
    s.handPoseL = makeAction("hand_pose_l", XR_ACTION_TYPE_POSE_INPUT, false);

    const struct {
        XrAction act;
        const char* path;
    } binds[] = {
        {s.move, "/user/hand/left/input/thumbstick"},
        {s.camera, "/user/hand/right/input/thumbstick"},
        {s.a, "/user/hand/right/input/a/click"},
        {s.b, "/user/hand/right/input/b/click"},
        {s.x, "/user/hand/left/input/x/click"},
        {s.y, "/user/hand/left/input/y/click"},
        {s.z, "/user/hand/right/input/squeeze/value"},
        {s.start, "/user/hand/left/input/menu/click"},
        {s.trigL, "/user/hand/left/input/trigger/value"},
        {s.trigR, "/user/hand/right/input/trigger/value"},
        {s.rumble, "/user/hand/left/output/haptic"},
        {s.rumble, "/user/hand/right/output/haptic"},
        {s.handPose, "/user/hand/right/input/grip/pose"},
        {s.aimPose, "/user/hand/left/input/aim/pose"},
        {s.aimPose, "/user/hand/right/input/aim/pose"},
        {s.stickClick, "/user/hand/right/input/thumbstick/click"},
        {s.gripL, "/user/hand/left/input/squeeze/value"},
        {s.handPoseL, "/user/hand/left/input/grip/pose"},
    };
    std::vector<XrActionSuggestedBinding> suggested;
    for (const auto& bnd : binds) suggested.push_back({bnd.act, toPath(bnd.path)});
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = toPath("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = uint32_t(suggested.size());
    sb.suggestedBindings = suggested.data();
    if (!ok(xrSuggestInteractionProfileBindings(s.instance, &sb), "xrSuggestInteractionProfileBindings"))
        return false;
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &s.actionSet;
    if (!ok(xrAttachSessionActionSets(s.session, &ai), "xrAttachSessionActionSets")) return false;
    XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    asi.action = s.handPose;
    asi.poseInActionSpace.orientation.w = 1.0f;
    ok(xrCreateActionSpace(s.session, &asi, &s.handSpace), "xrCreateActionSpace");
    asi.action = s.handPoseL;
    ok(xrCreateActionSpace(s.session, &asi, &s.handSpaceL), "xrCreateActionSpace (left grip)");
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo aim{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        aim.action = s.aimPose;
        aim.subactionPath = s.hands[h];
        aim.poseInActionSpace.orientation.w = 1.0f;
        ok(xrCreateActionSpace(s.session, &aim, &s.aimSpace[h]), "xrCreateActionSpace (aim)");
    }
    xrlog("Touch controller bindings ready");
    return true;
}

void setRefreshRate() {
    if (!s.hasRefreshRate || s.settings.refresh <= 0) return;
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB request = nullptr;
    xrGetInstanceProcAddr(s.instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&enumerate);
    xrGetInstanceProcAddr(s.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&request);
    if (!enumerate || !request) return;
    uint32_t n = 0;
    if (XR_FAILED(enumerate(s.session, 0, &n, nullptr)) || !n) return;
    std::vector<float> rates(n);
    enumerate(s.session, n, &n, rates.data());
    float best = 0.0f, lowest = rates[0];
    for (float r : rates) {
        lowest = r < lowest ? r : lowest;
        if (r <= float(s.settings.refresh) + 0.5f && r > best) best = r;
    }
    if (best <= 0.0f) best = lowest;
    if (ok(request(s.session, best), "xrRequestDisplayRefreshRateFB")) xrlog("refresh rate %.0f Hz", best);
}

// The GPU's clock (gpu_boost): the runtime keeps it at its sustained high level, or chooses (the
// game is GPU-bound in the diorama: both eyes and the HUD each frame). Applied when it changes.
void updatePerformance() {
    const int want = s.settings.gpuBoost ? 1 : 0;
    if (!s.hasPerf || want == s.perfApplied) return;
    s.perfApplied = want;
    PFN_xrPerfSettingsSetPerformanceLevelEXT setLevel = nullptr;
    xrGetInstanceProcAddr(s.instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction*)&setLevel);
    if (!setLevel) return;
    const XrPerfSettingsLevelEXT level = want ? XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
    if (ok(setLevel(s.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, level), "xrPerfSettingsSetPerformanceLevelEXT"))
        xrlog("GPU level: %s", want ? "sustained high" : "sustained low");
}

// The room as the bottom layer, started and paused as the setting changes. True while it shows.
bool updatePassthrough() {
    // the diorama and the window sit in the room
    const bool want = (s.settings.passthrough || s.settings.diorama || s.settings.window) && s.hasPassthrough;
    if (want == s.passthroughRunning) return want;
    if (want && !s.passthrough) {
        PFN_xrCreatePassthroughFB create = nullptr;
        PFN_xrCreatePassthroughLayerFB createLayer = nullptr;
        xrGetInstanceProcAddr(s.instance, "xrCreatePassthroughFB", (PFN_xrVoidFunction*)&create);
        xrGetInstanceProcAddr(s.instance, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction*)&createLayer);
        XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        pci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        if (!create || !createLayer || !ok(create(s.session, &pci, &s.passthrough), "xrCreatePassthroughFB")) {
            s.hasPassthrough = false;
            return false;
        }
        XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        lci.passthrough = s.passthrough;
        lci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
        if (!ok(createLayer(s.session, &lci, &s.passthroughLayer), "xrCreatePassthroughLayerFB")) {
            s.hasPassthrough = false;
            return false;
        }
        s.passthroughRunning = true;
        xrlog("passthrough on");
        return true;
    }
    PFN_xrPassthroughStartFB start = nullptr;
    PFN_xrPassthroughPauseFB pause = nullptr;
    PFN_xrPassthroughLayerResumeFB resumeLayer = nullptr;
    PFN_xrPassthroughLayerPauseFB pauseLayer = nullptr;
    xrGetInstanceProcAddr(s.instance, "xrPassthroughStartFB", (PFN_xrVoidFunction*)&start);
    xrGetInstanceProcAddr(s.instance, "xrPassthroughPauseFB", (PFN_xrVoidFunction*)&pause);
    xrGetInstanceProcAddr(s.instance, "xrPassthroughLayerResumeFB", (PFN_xrVoidFunction*)&resumeLayer);
    xrGetInstanceProcAddr(s.instance, "xrPassthroughLayerPauseFB", (PFN_xrVoidFunction*)&pauseLayer);
    if (!start || !pause || !resumeLayer || !pauseLayer) return false;
    if (want) {
        start(s.passthrough);
        resumeLayer(s.passthroughLayer);
    } else {
        pauseLayer(s.passthroughLayer);
        pause(s.passthrough);
    }
    s.passthroughRunning = want;
    xrlog("passthrough %s", want ? "on" : "off");
    return want;
}

void pollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(s.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& sc = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            s.state = sc.state;
            xrlog("session state %d", int(sc.state));
            switch (sc.state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                s.running = ok(xrBeginSession(s.session, &bi), "xrBeginSession");
                if (s.running) {
                    setRefreshRate();
                    aurora_xr_present_enable(true);
                    {
                        std::lock_guard lock{s_shared};
                        s_sharedRunning = true;
                    }
                    xrlog("session running; Aurora presents to the headset (device support %s)",
                          aurora_xr_present_supported() ? "yes" : "not yet known or no");
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                // The player closed the app or left it from the headset's menu (the Meta button's
                // menu alone only makes the session VISIBLE). The game would go on in the background,
                // music and all, and there is no window to come back to: it ends here, as on a quit.
                // Saves are written when the game saves, so nothing is lost; the next start is fresh.
                aurora_xr_present_enable(false);
                if (sc.state == XR_SESSION_STATE_STOPPING) xrEndSession(s.session);
                s.running = false;
                s.exiting = true;
                xrlog("session %s: the game ends", sc.state == XR_SESSION_STATE_STOPPING ? "stopping" : "exiting");
                pc_exit(0);
            default:
                break;
            }
            s.focused = sc.state == XR_SESSION_STATE_FOCUSED;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            aurora_xr_present_enable(false);
            s.running = false;
            s.exiting = true;
            xrlog("instance loss pending: the game ends");
            pc_exit(0);
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---- Controllers ---------------------------------------------------------------------------------

bool SDLCALL padRumble(void*, Uint16 low, Uint16 high) {
    s_rumbleWanted.store(low > high ? low : high);
    return true;
}

// The virtual gamepad, once SDL's joystick subsystem is up (Aurora initializes it). Buttons and
// axes in SDL's gamepad order, so SDL maps them as a standard gamepad.
void attachPad() {
    if (s.pad || !SDL_WasInit(SDL_INIT_JOYSTICK)) return;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
    desc.name = "Quest Touch (WakerQuest)";
    desc.Rumble = padRumble;
    s.padId = SDL_AttachVirtualJoystick(&desc);
    if (!s.padId) {
        xrlog("SDL_AttachVirtualJoystick failed: %s", SDL_GetError());
        return;
    }
    s.pad = SDL_OpenJoystick(s.padId);
    if (!s.pad) {
        xrlog("SDL_OpenJoystick (virtual) failed: %s", SDL_GetError());
        return;
    }
    // the triggers rest at the axis minimum
    SDL_SetJoystickVirtualAxis(s.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(s.pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    xrlog("virtual gamepad attached (instance %u)", unsigned(s.padId));
}

bool getBool(XrAction act) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &gi, &st)) && st.isActive && st.currentState;
}
float getFloat(XrAction act) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(s.session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
XrVector2f getVec2(XrAction act) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(s.session, &gi, &st)) && st.isActive) return st.currentState;
    return {0.0f, 0.0f};
}
Sint16 stickAxis(float v) { return Sint16(clampf(v, -1.0f, 1.0f) * 32767.0f); }
Sint16 triggerAxis(float v) { return Sint16(-32768 + int(clampf(v, 0.0f, 1.0f) * 65535.0f)); }

void publishSettings() {
    std::lock_guard lock{s_shared};
    s_sharedSettings = s.settings;
}

void updateGrips(float gripL, float gripR, bool click);
void togglePanel();
void pointerHaptic(int hand, float amplitude);

void updatePad() {
    attachPad();
    if (!s.pad) return;
    XrVector2f m{0, 0}, c{0, 0};
    float l = 0, r = 0, z = 0;
    bool a = false, b = false, x = false, y = false, start = false;
    if (s.focused) {
        XrActiveActionSet active{s.actionSet, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &active;
        if (XR_SUCCEEDED(xrSyncActions(s.session, &si))) {
            m = getVec2(s.move);
            c = getVec2(s.camera);
            l = getFloat(s.trigL);
            r = getFloat(s.trigR);
            z = getFloat(s.z);
            a = getBool(s.a);
            b = getBool(s.b);
            x = getBool(s.x);
            y = getBool(s.y);
            start = getBool(s.start);
            s.trig[0] = l;
            s.trig[1] = r;
            const float gripL = getFloat(s.gripL);
            const bool menu = s_menuOpen.load();
            // both triggers and the right grip: the settings panel, opened or closed (the game's own
            // options menu, open, keeps the combination: it closes it)
            const bool combo = !menu && l > 0.5f && r > 0.5f && z > 0.6f;
            if (combo && !s.comboWas) togglePanel();
            s.comboWas = combo;
            static bool panelWas = false;
            const bool panel = wqp::isOpen();
            // from the combination, or the panel closing, until every trigger and grip is let go: not
            // the game's
            if (combo || (panelWas && !panel)) s.comboHeld = true;
            if (s.comboHeld && l < 0.2f && r < 0.2f && z < 0.2f) s.comboHeld = false;
            panelWas = panel;
            if (panel) {
                bool tick = false;
                wqp::nav(m.x, m.y, c.x, a, b, &tick);
                if (tick) pointerHaptic(s.activeHand, 0.15f);
            }
            if (panel || s.comboHeld) {
                m = c = {0.0f, 0.0f};
                l = r = z = 0.0f;
                a = b = x = y = start = false;
            } else if (menu) {
                // the options menu: the triggers are the laser pointer's clicks, not L and R
                l = r = 0.0f;
                z = 0.0f;
            } else if (s.settings.diorama || s.settings.window) {
                // the diorama: the grips move the model or a panel (not the game's Z); the window: the
                // window
                updateGrips(gripL, z, getBool(s.stickClick));
                z = 0.0f;
            }
        }
    }
    // The diorama, controls relative to the view: the left stick turned from the player's heading on the
    // model to the game camera's, so pushing it forward takes Link away from the player whichever way
    // the model or the camera has turned. The player's heading: from the head to the model's middle
    // (the head's own forward when it is nearly above it), smoothed.
    if (s.settings.diorama && s.settings.viewRelative && s_camHeadingValid.load() && s_rotateStick.load() &&
        !s_menuOpen.load()) {
        WqXrHead head{};
        {
            std::lock_guard lock{s_shared};
            head = s_heads[(s_nextTag - 1) % kHeadHistory].head;
        }
        if (head.tag != 0) {
            float vx = s.settings.tableX - head.pos[0], vz = -s.settings.tableDistance - head.pos[2];
            if (vx * vx + vz * vz < 0.3f * 0.3f) {
                const float x = head.rot[0], y = head.rot[1], z = head.rot[2], w = head.rot[3];
                vx = -2.0f * (x * z + w * y);
                vz = -(1.0f - 2.0f * (x * x + y * y));
            }
            const float viewHeading = std::atan2(vx, -vz);
            if (!s_viewHeadingValid) {
                s_viewHeading = viewHeading;
                s_viewHeadingValid = true;
            }
            float d = viewHeading - s_viewHeading;
            while (d > 3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            s_viewHeading += 0.1f * d;
            const float delta = s_viewHeading - s_camHeading.load();
            const float cs = std::cos(delta), sn = std::sin(delta);
            const XrVector2f turned{m.x * cs + m.y * sn, -m.x * sn + m.y * cs};
            m = turned;
        }
    }
    // SDL's Y axes point down, OpenXR's up
    SDL_SetJoystickVirtualAxis(s.pad, SDL_GAMEPAD_AXIS_LEFTX, stickAxis(m.x));
    SDL_SetJoystickVirtualAxis(s.pad, SDL_GAMEPAD_AXIS_LEFTY, stickAxis(-m.y));
    SDL_SetJoystickVirtualAxis(s.pad, SDL_GAMEPAD_AXIS_RIGHTX, stickAxis(c.x));
    SDL_SetJoystickVirtualAxis(s.pad, SDL_GAMEPAD_AXIS_RIGHTY, stickAxis(-c.y));
    SDL_SetJoystickVirtualAxis(s.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, triggerAxis(l));
    SDL_SetJoystickVirtualAxis(s.pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, triggerAxis(r));
    SDL_SetJoystickVirtualButton(s.pad, SDL_GAMEPAD_BUTTON_SOUTH, a);
    SDL_SetJoystickVirtualButton(s.pad, SDL_GAMEPAD_BUTTON_EAST, b);
    SDL_SetJoystickVirtualButton(s.pad, SDL_GAMEPAD_BUTTON_WEST, x);
    SDL_SetJoystickVirtualButton(s.pad, SDL_GAMEPAD_BUTTON_NORTH, y);
    SDL_SetJoystickVirtualButton(s.pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, z > 0.6f);
    SDL_SetJoystickVirtualButton(s.pad, SDL_GAMEPAD_BUTTON_START, start);
}

void updateRumble() {
    const int want = s.running ? s_rumbleWanted.load() : 0;
    if (want == s.rumbleNow) return;
    s.rumbleNow = want;
    for (XrPath hand : s.hands) {
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = s.rumble;
        hi.subactionPath = hand;
        if (want <= 0) {
            xrStopHapticFeedback(s.session, &hi);
            continue;
        }
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
        v.amplitude = float(want) / 65535.0f;
        v.duration = 5000000000LL;  // 5 s; SDL stops it (Rumble with 0) when Aurora's duration ends
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        xrApplyHapticFeedback(s.session, &hi, (const XrHapticBaseHeader*)&v);
    }
}

// ---- The options menu's laser pointer ---------------------------------------------------------------

XrVector3f vadd(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
XrVector3f vsub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
XrVector3f vscale(XrVector3f a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float vdot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
XrVector3f vcross(XrVector3f a, XrVector3f b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
XrVector3f vnorm(XrVector3f a) {
    const float l = std::sqrt(vdot(a, a));
    return l > 1e-6f ? vscale(a, 1.0f / l) : XrVector3f{0, 0, -1};
}
XrVector3f qrot(const XrQuaternionf& q, XrVector3f v) {
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f t = vscale(vcross(u, v), 2.0f);
    return vadd(vadd(v, vscale(t, q.w)), vcross(u, t));
}
// a quaternion from orthonormal axes (the columns of a rotation)
XrQuaternionf qfromAxes(XrVector3f x, XrVector3f y, XrVector3f z) {
    XrQuaternionf q;
    const float tr = x.x + y.y + z.z;
    if (tr > 0.0f) {
        const float s4 = std::sqrt(tr + 1.0f) * 2.0f;
        q = {(y.z - z.y) / s4, (z.x - x.z) / s4, (x.y - y.x) / s4, 0.25f * s4};
    } else if (x.x > y.y && x.x > z.z) {
        const float s4 = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
        q = {0.25f * s4, (y.x + x.y) / s4, (z.x + x.z) / s4, (y.z - z.y) / s4};
    } else if (y.y > z.z) {
        const float s4 = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
        q = {(y.x + x.y) / s4, 0.25f * s4, (z.y + y.z) / s4, (z.x - x.z) / s4};
    } else {
        const float s4 = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
        q = {(z.x + x.z) / s4, (z.y + y.z) / s4, 0.25f * s4, (x.y - y.x) / s4};
    }
    return q;
}

// A static swapchain drawn once: the cursor (a soft white disc) or the ray (white, fading along it).
XrSwapchain makePointerTexture(uint32_t w, uint32_t h, int mode) {
    static GLuint prog = 0;
    static GLint uMode = -1;
    if (!prog) {
        static const char* vs = R"(#version 300 es
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
        static const char* fs = R"(#version 300 es
precision highp float;
uniform int u_mode;
in vec2 v_uv;
out vec4 o_color;
void main() {
    float a;
    if (u_mode == 0) {
        float r = length(v_uv - 0.5) * 2.0;
        a = 1.0 - smoothstep(0.75, 1.0, r);
    } else {
        float across = 1.0 - smoothstep(0.3, 1.0, abs(v_uv.y - 0.5) * 2.0);
        a = across * (1.0 - v_uv.x) * 0.8;
    }
    o_color = vec4(a, a, a, a);  // premultiplied white
}
)";
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, f);
        glLinkProgram(prog);
        glDeleteShader(v);
        glDeleteShader(f);
        uMode = glGetUniformLocation(prog, "u_mode");
    }
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sc.format = s.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    sc.sampleCount = 1;
    sc.width = w;
    sc.height = h;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    XrSwapchain chain = XR_NULL_HANDLE;
    if (!ok(xrCreateSwapchain(s.session, &sc, &chain), "xrCreateSwapchain (pointer)")) return XR_NULL_HANDLE;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(chain, 0, &n, nullptr);
    std::vector<XrSwapchainImageOpenGLESKHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(chain, n, &n, (XrSwapchainImageBaseHeader*)images.data());
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_SUCCEEDED(xrAcquireSwapchainImage(chain, &acq, &idx)) && XR_SUCCEEDED(xrWaitSwapchainImage(chain, &wait))) {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[idx].image, 0);
        glViewport(0, 0, GLsizei(w), GLsizei(h));
        glDisable(GL_BLEND);
        glUseProgram(prog);
        glUniform1i(uMode, mode);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(chain, &rel);
    }
    return chain;
}

void pointerHaptic(int hand, float amplitude) {
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = s.rumble;
    hi.subactionPath = s.hands[hand];
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude;
    v.duration = 15000000;  // 15 ms: a tick
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    xrApplyHapticFeedback(s.session, &hi, (const XrHapticBaseHeader*)&v);
}

void pushMouse(Uint32 type, float x, float y, bool down) {
    SDL_Event e{};
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.button.x = x;
        e.button.y = y;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = down;
        e.button.clicks = 1;
    }
    SDL_PushEvent(&e);
}

void ensurePointerTextures() {
    if (!s.cursorSwapchain) s.cursorSwapchain = makePointerTexture(64, 64, 0);
    if (!s.raySwapchain) s.raySwapchain = makePointerTexture(256, 16, 1);
}

// The triggers this frame: which hand points (the last to pull its trigger).
void updateTriggers() {
    for (int h = 0; h < 2; h++) {
        const bool down = s.trig[h] > (s.trigDown[h] ? 0.4f : 0.6f);
        if (down && !s.trigDown[h] && h != s.activeHand) s.activeHand = h;
        s.trigDown[h] = down;
    }
}

// Hand h's aim ray in the LOCAL space.
bool aimRay(int h, XrVector3f* o, XrVector3f* d) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (!s.aimSpace[h] || XR_FAILED(xrLocateSpace(s.aimSpace[h], s.space, s.frame.predictedDisplayTime, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return false;
    *o = loc.pose.position;
    *d = vnorm(qrot(loc.pose.orientation, {0, 0, -1}));
    return true;
}

// Where a ray meets a quad (either side): its distance, and the point in the quad (u, v from the top
// left, 0..1). False when it misses.
bool rayQuad(XrVector3f o, XrVector3f d, const XrPosef& pose, XrExtent2Df size, float* t, float* u = nullptr,
             float* v = nullptr) {
    const XrVector3f n = qrot(pose.orientation, {0, 0, 1});
    const float dn = vdot(d, n);
    if (std::fabs(dn) < 1e-4f) return false;
    const float tt = vdot(vsub(pose.position, o), n) / dn;
    if (tt <= 0.0f) return false;
    const XrQuaternionf inv{-pose.orientation.x, -pose.orientation.y, -pose.orientation.z, pose.orientation.w};
    const XrVector3f local = qrot(inv, vsub(vadd(o, vscale(d, tt)), pose.position));
    const float uu = local.x / size.width + 0.5f, vv = 0.5f - local.y / size.height;
    if (uu < 0.0f || uu > 1.0f || vv < 0.0f || vv > 1.0f) return false;
    *t = tt;
    if (u) *u = uu;
    if (v) *v = vv;
    return true;
}

XrVector3f latestHeadPos() {
    std::lock_guard lock{s_shared};
    const WqXrHead& h = s_heads[(s_nextTag - 1) % kHeadHistory].head;
    return {h.pos[0], h.pos[1], h.pos[2]};
}

// A ray from o along d, `len` long and `width` wide: a thin strip turned to face the eyes.
void rayLayer(XrVector3f o, XrVector3f d, float len, float width, XrCompositionLayerQuad* out) {
    const XrVector3f mid = vadd(o, vscale(d, 0.5f * len));
    XrVector3f z = vsub(latestHeadPos(), mid);
    z = vnorm(vsub(z, vscale(d, vdot(z, d))));
    const XrVector3f y = vcross(z, d);
    *out = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = s.space;
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = s.raySwapchain;
    out->subImage.imageRect = {{0, 0}, {256, 16}};
    out->pose.orientation = qfromAxes(d, y, z);
    out->pose.position = mid;
    out->size = {len, width};
}

// The cursor where the ray meets a panel (larger over something to press).
void cursorLayer(XrVector3f at, const XrQuaternionf& orientation, bool big, XrCompositionLayerQuad* out) {
    const XrVector3f n = qrot(orientation, {0, 0, 1});
    *out = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = s.space;
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = s.cursorSwapchain;
    out->subImage.imageRect = {{0, 0}, {64, 64}};
    out->pose.orientation = orientation;
    out->pose.position = vadd(at, vscale(n, 0.003f));
    const float c = big ? 0.022f : 0.014f;
    out->size = {c, c};
}

// The game's options menu (ImGui) on a panel: the active hand's ray against it, the mouse events for
// ImGui, haptic ticks, and the ray's and the cursor's layers. Returns the layers written.
int updatePointer(const XrPosef& panel, XrExtent2Df size, XrCompositionLayerQuad* out) {
    ensurePointerTextures();
    const int h = s.activeHand;
    XrVector3f o, d;
    if (!aimRay(h, &o, &d)) return 0;
    float len = 2.0f, u = 0.0f, v = 0.0f;
    const bool onPanel = rayQuad(o, d, panel, size, &len, &u, &v);
    if (!onPanel) len = 2.0f;
    if (onPanel) {
        const float mx = u * s_menuW.load(), my = v * s_menuH.load();
        if (std::fabs(mx - s.mouseX) > 0.5f || std::fabs(my - s.mouseY) > 0.5f) {
            pushMouse(SDL_EVENT_MOUSE_MOTION, mx, my, false);
            s.mouseX = mx;
            s.mouseY = my;
        }
    }
    const bool click = s.trigDown[h];
    if (click != s.clickDown && (onPanel || s.clickDown)) {
        pushMouse(click ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP, s.mouseX, s.mouseY, click);
        if (click) pointerHaptic(h, 0.5f);
        s.clickDown = click;
    }
    const bool hovered = onPanel && s_menuHovered.load();
    if (hovered && !s.hoverWas) pointerHaptic(h, 0.15f);
    s.hoverWas = hovered;
    int layers = 0;
    if (s.raySwapchain) rayLayer(o, d, len, 0.004f, &out[layers++]);
    if (onPanel && s.cursorSwapchain) cursorLayer(vadd(o, vscale(d, len)), panel.orientation, hovered, &out[layers++]);
    return layers;
}

// The settings panel (wq_panel.cpp): the active hand's ray and trigger, its ticks; the ray's and the
// cursor's layers. Returns the layers written.
int updatePanelPointer(XrCompositionLayerQuad* out) {
    ensurePointerTextures();
    const int h = s.activeHand;
    XrVector3f o, d;
    if (!aimRay(h, &o, &d)) return 0;
    bool tick = false, hovering = false;
    const float t = wqp::pointer(o, d, s.trigDown[h], &tick, &hovering);
    if (tick) pointerHaptic(h, s.trigDown[h] ? 0.5f : 0.15f);
    int layers = 0;
    XrCompositionLayerQuad panel;
    const bool shown = wqp::layer(s.space, &panel);
    if (s.raySwapchain) rayLayer(o, d, t > 0.0f ? t : 2.0f, 0.004f, &out[layers++]);
    if (t > 0.0f && shown && s.cursorSwapchain) cursorLayer(vadd(o, vscale(d, t)), panel.pose.orientation, hovering, &out[layers++]);
    return layers;
}

// The diorama with no menu open: a faint ray from a hand whose aim is on the HUD's or the dialogues'
// panel (a grip there takes the panel). Returns the layers written.
int updatePanelRays(XrCompositionLayerQuad* out) {
    ensurePointerTextures();
    int layers = 0;
    for (int h = 0; h < 2; h++) {
        XrVector3f o, d;
        if (!s.raySwapchain || !aimRay(h, &o, &d)) continue;
        float t = 0.0f, best = 0.0f;
        if (s.hudShown && rayQuad(o, d, s.hudPanel, s.hudSize, &t)) best = t;
        if (s.dialogueShown && rayQuad(o, d, s.dialoguePanel, s.dialogueSize, &t) && (best == 0.0f || t < best)) best = t;
        if (best > 0.0f || s.grabbed[h]) rayLayer(o, d, best > 0.0f ? best : 0.3f, 0.0022f, &out[layers++]);
    }
    return layers;
}

// The panel a hand takes with its grip: the one its ray is on, or its hand is at. 0 none, 1 the HUD,
// 2 the dialogues.
int panelUnderHand(int h, XrVector3f hand) {
    XrVector3f o, d;
    float t = 0.0f, best = 0.0f;
    int which = 0;
    if (aimRay(h, &o, &d)) {
        if (s.hudShown && rayQuad(o, d, s.hudPanel, s.hudSize, &t)) best = t, which = 1;
        if (s.dialogueShown && rayQuad(o, d, s.dialoguePanel, s.dialogueSize, &t) && (best == 0.0f || t < best)) which = 2;
        if (which) return which;
    }
    // the hand itself at a panel: within 8 cm of it, in front or behind
    auto near = [&](const XrPosef& pose, XrExtent2Df size) {
        const XrQuaternionf inv{-pose.orientation.x, -pose.orientation.y, -pose.orientation.z, pose.orientation.w};
        const XrVector3f l = qrot(inv, vsub(hand, pose.position));
        return std::fabs(l.z) < 0.08f && std::fabs(l.x) < 0.5f * size.width && std::fabs(l.y) < 0.5f * size.height;
    };
    if (s.dialogueShown && near(s.dialoguePanel, s.dialogueSize)) return 2;
    if (s.hudShown && near(s.hudPanel, s.hudSize)) return 1;
    return 0;
}

float headingOf(const XrQuaternionf& q) {
    // the forward (-z) on the ground: its heading, positive to the right
    const float fx = -2.0f * (q.x * q.z + q.w * q.y), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return std::atan2(fx, -fz);
}

float wrapPi(float a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

void saveModel() {
    const Settings& st = s.settings;
    char v[32];
    snprintf(v, sizeof v, "%.3f", st.tableX);
    wq_xr_set("bubble_x", v);
    snprintf(v, sizeof v, "%.3f", st.tableHeight);
    wq_xr_set("bubble_height", v);
    snprintf(v, sizeof v, "%.3f", st.tableDistance);
    wq_xr_set("bubble_distance", v);
    snprintf(v, sizeof v, "%.1f", st.tableYaw);
    wq_xr_set("bubble_yaw", v);
    snprintf(v, sizeof v, "%.2f", st.tableWidth);
    wq_xr_set("bubble_size", v);
}

void saveWindow() {
    const Settings& st = s.settings;
    char v[32];
    snprintf(v, sizeof v, "%.3f", st.windowX);
    wq_xr_set("window_x", v);
    snprintf(v, sizeof v, "%.3f", st.windowHeight);
    wq_xr_set("window_height", v);
    snprintf(v, sizeof v, "%.3f", st.windowDistance);
    wq_xr_set("window_distance", v);
    snprintf(v, sizeof v, "%.1f", st.windowYaw);
    wq_xr_set("window_yaw", v);
    snprintf(v, sizeof v, "%.2f", st.windowWidth);
    wq_xr_set("window_width", v);
}

// The window's pose in the room (its middle, turned about the vertical) and its half sizes.
XrPosef windowPose(const Settings& st) {
    const float yaw = st.windowYaw * 0.0174532925f;
    XrPosef p;
    p.orientation = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
    p.position = {st.windowX, st.windowHeight, -st.windowDistance};
    return p;
}

// A point of the window's own space (x right, y up, z toward the player) in the room.
XrVector3f onWindow(const Settings& st, float x, float y, float z) {
    const XrPosef p = windowPose(st);
    return vadd(p.position, qrot(p.orientation, {x, y, z}));
}

// The diorama's grips. Pressed with the hand's ray (or the hand) on the HUD's or the dialogues' panel:
// the panel follows the hand (the HUD turned to face the player), saved on release. Otherwise on the
// model: one hand moves it and turns it about the vertical (the right stick's click while holding
// puts it back); both hands move, turn and scale it (its size, the world inside the same), with a
// tick every 10% and its size shown. Saved to xr.ini on release.
void updateGrips(float gripL, float gripR, bool click) {
    XrSpaceLocation loc[2] = {{XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}};
    const XrSpace spaces[2] = {s.handSpaceL, s.handSpace};
    bool valid[2];
    for (int h = 0; h < 2; h++) {
        valid[h] = spaces[h] && XR_SUCCEEDED(xrLocateSpace(spaces[h], s.space, s.frame.predictedDisplayTime, &loc[h])) &&
                   (loc[h].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                   (loc[h].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    }
    Settings& st = s.settings;
    const float g[2] = {gripL, gripR};
    for (int h = 0; h < 2; h++) {
        const bool down = valid[h] && g[h] > (s.gripDown[h] ? 0.4f : 0.6f);
        if (down && !s.gripDown[h]) {
            const int panel = panelUnderHand(h, loc[h].pose.position);
            if (panel) {
                s.grabbed[h] = panel;
                s.grabHand[h] = loc[h].pose.position;
                const float* from = panel == 1 ? st.hudPose : st.dialogueOffset;
                for (int i = 0; i < 3; i++) s.grabOffset[h][i] = from[i];
                pointerHaptic(h, 0.3f);
                xrlog("grip: moving the %s", panel == 1 ? "HUD" : "dialogues");
            }
        }
        if (!down && s.grabbed[h]) {
            char v[64];
            if (s.grabbed[h] == 1) {
                snprintf(v, sizeof v, "%.3f,%.3f,%.3f,%.1f", st.hudPose[0], st.hudPose[1], st.hudPose[2], st.hudPose[3]);
                wq_xr_set("hud_pose", v);
            } else {
                snprintf(v, sizeof v, "%.3f,%.3f,%.3f", st.dialogueOffset[0], st.dialogueOffset[1], st.dialogueOffset[2]);
                wq_xr_set("dialogue_offset", v);
            }
            s.grabbed[h] = 0;
        }
        s.gripDown[h] = down;
        if (s.grabbed[h] && valid[h]) {
            const XrVector3f dp = vsub(loc[h].pose.position, s.grabHand[h]);
            float* to = s.grabbed[h] == 1 ? st.hudPose : st.dialogueOffset;
            to[0] = s.grabOffset[h][0] + dp.x;
            to[1] = s.grabOffset[h][1] + dp.y;
            to[2] = s.grabOffset[h][2] + dp.z;
            if (s.grabbed[h] == 1) {
                const XrVector3f head = latestHeadPos();
                st.hudPose[3] = std::atan2(head.x - to[0], head.z - to[2]) * 57.2957795f;
            }
            publishSettings();
        }
    }
    // the model: the hands that hold no panel
    const int hands = (s.gripDown[0] && !s.grabbed[0] ? 1 : 0) | (s.gripDown[1] && !s.grabbed[1] ? 2 : 0);
    if (hands != s.dragHands) {
        if (s.dragHands && !hands) st.window ? saveWindow() : saveModel();
        if (hands)
            xrlog("grip: %s", st.window ? (hands == 3 ? "both hands on the window" : "moving the window")
                              : hands == 3 ? "both hands on the model" : "moving the model");
        s.dragHands = hands;
        s.dragging = false;  // starts again from the hands as they are
    }
    if (!hands) {
        s.clickWas = click;
        return;
    }
    XrVector3f at;
    float heading, span = 0.0f;
    if (hands == 3) {
        const XrVector3f a = loc[0].pose.position, b = loc[1].pose.position;
        at = vscale(vadd(a, b), 0.5f);
        const XrVector3f v = vsub(b, a);
        heading = std::atan2(v.z, v.x);  // the hands' line turned right: the same sign as a hand's heading
        span = std::sqrt(vdot(v, v));
    } else {
        const XrPosef& pose = loc[hands == 1 ? 0 : 1].pose;
        at = pose.position;
        heading = headingOf(pose.orientation);
    }
    if (st.window) {
        // the window: one hand moves it and turns it about the vertical (the right stick's click while
        // holding puts it back), both hands also change its width
        if (!s.dragging) {
            s.dragging = true;
            s.dragHand = at;
            s.dragHandYaw = heading;
            s.dragX = st.windowX;
            s.dragHeight = st.windowHeight;
            s.dragDistance = st.windowDistance;
            s.dragYaw = st.windowYaw;
            s.dragSpan = span;
            s.dragSize = s.tickSize = st.windowWidth;
        }
        if (hands == 2 && click && !s.clickWas) {
            const Settings defaults;
            st.windowX = defaults.windowX;
            st.windowHeight = defaults.windowHeight;
            st.windowDistance = defaults.windowDistance;
            st.windowYaw = defaults.windowYaw;
            s.dragging = false;
            xrlog("grip: the window back in its place");
        } else {
            st.windowX = s.dragX + (at.x - s.dragHand.x);
            st.windowHeight = s.dragHeight + (at.y - s.dragHand.y);
            st.windowDistance = std::max(0.3f, s.dragDistance - (at.z - s.dragHand.z));
            st.windowYaw = s.dragYaw - wrapPi(heading - s.dragHandYaw) * 57.2957795f;
            if (hands == 3 && s.dragSpan > 0.05f) {
                st.windowWidth = clampf(s.dragSize * span / s.dragSpan, 0.3f, 6.0f);
                if (std::fabs(st.windowWidth / s.tickSize - 1.0f) >= 0.1f) {
                    s.tickSize = st.windowWidth;
                    pointerHaptic(0, 0.25f);
                    pointerHaptic(1, 0.25f);
                }
                s.sizeShownUntil = SDL_GetTicks() + 1500;
            }
        }
        publishSettings();
        s.clickWas = click;
        return;
    }
    if (!s.dragging) {
        s.dragging = true;
        s.dragHand = at;
        s.dragHandYaw = heading;
        s.dragX = st.tableX;
        s.dragHeight = st.tableHeight;
        s.dragDistance = st.tableDistance;
        s.dragYaw = st.tableYaw;
        s.dragSpan = span;
        s.dragSize = s.tickSize = st.tableWidth;
    }
    if (hands == 2 && click && !s.clickWas) {
        st.tableX = 0.0f;
        st.tableHeight = 0.3f;
        st.tableDistance = 1.0f;
        st.tableYaw = 0.0f;
        s.dragging = false;
        xrlog("grip: the model back in its place");
    } else {
        st.tableX = s.dragX + (at.x - s.dragHand.x);
        st.tableHeight = s.dragHeight - (at.y - s.dragHand.y);
        st.tableDistance = std::max(0.2f, s.dragDistance - (at.z - s.dragHand.z));
        st.tableYaw = s.dragYaw - wrapPi(heading - s.dragHandYaw) * 57.2957795f;
        if (hands == 3 && s.dragSpan > 0.05f) {
            st.tableWidth = clampf(s.dragSize * span / s.dragSpan, 0.4f, 3.0f);
            if (std::fabs(st.tableWidth / s.tickSize - 1.0f) >= 0.1f) {
                s.tickSize = st.tableWidth;
                pointerHaptic(0, 0.25f);
                pointerHaptic(1, 0.25f);
            }
            s.sizeShownUntil = SDL_GetTicks() + 1500;
        }
    }
    publishSettings();
    s.clickWas = click;
}

// The settings panel opens 0.9 m in front of the player, a little below the eyes, facing them.
void togglePanel() {
    if (!s.panelReady) return;
    if (wqp::isOpen()) {
        wqp::close();
        return;
    }
    WqXrHead head{};
    {
        std::lock_guard lock{s_shared};
        head = s_heads[(s_nextTag - 1) % kHeadHistory].head;
    }
    const XrQuaternionf q{head.rot[0], head.rot[1], head.rot[2], head.rot[3]};
    XrVector3f f = qrot(q, {0, 0, -1});
    f.y = 0.0f;
    f = vnorm(f);
    const float yaw = std::atan2(-f.x, -f.z);
    XrPosef pose;
    pose.orientation = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
    pose.position = {head.pos[0] + 0.9f * f.x, head.pos[1] - 0.12f, head.pos[2] + 0.9f * f.z};
    wqp::open(pose);
}

// The panel's host (wq_panel.h): xr.ini's settings.
std::string hostGet(const char* key) { return settingValue(s.settings, key); }
void hostSet(const char* key, const char* value) {
    applySetting(s.settings, key, value);  // shown at once; the file is read again next frame
    publishSettings();
    wq_xr_set(key, value);
}
void hostResetPosition() {
    hostSet("bubble_x", "0");
    hostSet("bubble_height", "0.3");
    hostSet("bubble_distance", "1.0");
    hostSet("bubble_yaw", "0");
    // and the window
    const Settings defaults;
    char v[32];
    snprintf(v, sizeof v, "%g", defaults.windowX);
    hostSet("window_x", v);
    snprintf(v, sizeof v, "%g", defaults.windowHeight);
    hostSet("window_height", v);
    snprintf(v, sizeof v, "%g", defaults.windowDistance);
    hostSet("window_distance", v);
    snprintf(v, sizeof v, "%g", defaults.windowYaw);
    hostSet("window_yaw", v);
}
void hostResetPanels() {
    const Settings defaults;
    char v[64];
    snprintf(v, sizeof v, "%.3f,%.3f,%.3f,%.1f", defaults.hudPose[0], defaults.hudPose[1], defaults.hudPose[2],
             defaults.hudPose[3]);
    hostSet("hud_pose", v);
    hostSet("dialogue_offset", "0,0,0");
}

// ---- Head poses (the diorama) ----------------------------------------------------------------------

// The eyes where the runtime predicts them for this frame: the midpoint, the head's orientation, the
// distance between them and one frustum that holds both eyes' (with a margin for the head turning
// before a frame drawn now is shown; the compositor reprojects the rest), under a new tag.
void locateHead() {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = s.frame.predictedDisplayTime;
    li.space = s.space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(s.session, &li, &vs, 2, &n, views)) || n != 2) return;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        return;
    HeadSnap h;
    const XrVector3f& a = views[0].pose.position;
    const XrVector3f& b = views[1].pose.position;
    h.head.pos[0] = 0.5f * (a.x + b.x);
    h.head.pos[1] = 0.5f * (a.y + b.y);
    h.head.pos[2] = 0.5f * (a.z + b.z);
    const XrQuaternionf& q = views[0].pose.orientation;
    h.head.rot[0] = q.x;
    h.head.rot[1] = q.y;
    h.head.rot[2] = q.z;
    h.head.rot[3] = q.w;
    const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    h.head.ipd = d > 0.04f && d < 0.09f ? d : 0.063f;
    const float margin = 1.12f;
    h.head.tanLeft = std::min(std::tan(views[0].fov.angleLeft), std::tan(views[1].fov.angleLeft)) * margin;
    h.head.tanRight = std::max(std::tan(views[0].fov.angleRight), std::tan(views[1].fov.angleRight)) * margin;
    h.head.tanDown = std::min(std::tan(views[0].fov.angleDown), std::tan(views[1].fov.angleDown)) * margin;
    h.head.tanUp = std::max(std::tan(views[0].fov.angleUp), std::tan(views[1].fov.angleUp)) * margin;
    h.eyes[0] = views[0].pose;
    h.eyes[1] = views[1].pose;
    std::lock_guard lock{s_shared};
    h.head.tag = s_nextTag++;
    if (s_nextTag == 0) s_nextTag = 1;
    s_heads[h.head.tag % kHeadHistory] = h;
}

// The pose a diorama frame was drawn for (the newest if that one is gone).
bool headForTag(uint32_t tag, HeadSnap& out) {
    std::lock_guard lock{s_shared};
    const HeadSnap& h = s_heads[tag % kHeadHistory];
    if (tag != 0 && h.head.tag == tag) {
        out = h;
        return true;
    }
    const HeadSnap& latest = s_heads[(s_nextTag - 1) % kHeadHistory];
    if (latest.head.tag == 0) return false;
    out = latest;
    return true;
}

// ---- Frames --------------------------------------------------------------------------------------

// The newest Aurora frame into the screen's swapchain. True while the swapchain holds a picture.
bool copyNewestFrame() {
    if (s.pendingRead != EGL_NO_SYNC_KHR) {
        // the last copy has long run by now; Aurora may write that buffer again once it has
        s.clientWaitSync(s.display, s.pendingRead, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 50000000);
        s.destroySync(s.display, s.pendingRead);
        s.pendingRead = EGL_NO_SYNC_KHR;
        aurora_xr_present_done();
    }
    AuroraXrFrame f{};
    if (!aurora_xr_present_take(&f)) return s.haveScreen;
    if (s.lastSerial != 0 && f.serial > s.lastSerial + 1) s.skipped += f.serial - s.lastSerial - 1;
    s.lastSerial = f.serial;
    if (f.syncFd < 0) s.framesWithoutFence++;
    if (f.syncFd >= 0) {
        // the GPU waits for Aurora's rendering of the frame; EGL owns the file from here
        const EGLint attr[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, f.syncFd, EGL_NONE};
        EGLSyncKHR fence = s.createSync(s.display, EGL_SYNC_NATIVE_FENCE_ANDROID, attr);
        if (fence != EGL_NO_SYNC_KHR) {
            s.waitSync(s.display, fence, 0);
            s.destroySync(s.display, fence);
        } else {
            close(f.syncFd);
            glFinish();
        }
    }
    const GLuint tex = textureFor(f.buffer);
    bool copied = false;
    if (tex && ensureSwapchain(f.width, f.height)) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (ok(xrAcquireSwapchainImage(s.swapchain, &acq, &idx), "xrAcquireSwapchainImage") &&
            ok(xrWaitSwapchainImage(s.swapchain, &wait), "xrWaitSwapchainImage")) {
            glBindFramebuffer(GL_FRAMEBUFFER, s.fbos[idx]);
            glViewport(0, 0, GLsizei(s.screenW), GLsizei(s.screenH));
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glUseProgram(s.copyProg);
            glUniform1i(s.copyULinear, s.srgb ? 1 : 0);
            glUniform1i(s.copyUAlpha, f.hud ? 1 : 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(s.swapchain, &rel);
            copied = true;
        }
    }
    s.pendingRead = s.createSync(s.display, EGL_SYNC_FENCE_KHR, nullptr);
    glFlush();
    if (s.pendingRead == EGL_NO_SYNC_KHR) {
        glFinish();
        aurora_xr_present_done();
    }
    if (copied) {
        if (f.stereo != s.stereoFrame || f.sideBySide != s.sideBySide)
            xrlog("picture: %s", f.stereo ? "3D (one eye per half)" : f.sideBySide ? "flat (left half, a menu is open)" : "flat");
        if (f.diorama != s.dioramaFrame) xrlog("picture: %s", f.diorama ? "the diorama" : "the screen");
        s.sideBySide = f.sideBySide;
        s.stereoFrame = f.stereo;
        s.dioramaFrame = f.diorama;
        s.frameTag = f.tag;
        s.hudFrame = f.hud;
        s.haveScreen = true;
        if (++s.frames == 1 || s.frames % 600 == 0)
            xrlog("frame %u from Aurora (serial %u, %ux%u): %u skipped, %u without a fence", s.frames, f.serial,
                  f.width, f.height, s.skipped, s.framesWithoutFence);
    }
    return s.haveScreen;
}

// A quad's orientation at `at` turned about the vertical to face the player.
XrQuaternionf facingPlayer(XrVector3f at) {
    const XrVector3f head = latestHeadPos();
    const float yaw = std::atan2(head.x - at.x, head.z - at.z);
    return {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
}

// The giant screen's 3D (the diorama sets its own eye shift every frame, pc_vr_diorama.cpp): the
// settings', with more depth behind the screen (x1.6, the same plane) during
// cutscenes (cine_3d), faded in and out over half a second.
void updateScreenStereo() {
    const Settings& st = s.settings;
    if ((st.diorama || st.window) && s.dioramaFrame) {
        s_stereoApplied[0] = -1.0f;  // the diorama's eye shift; the screen's applied again after it
        return;
    }
    const float dt = s.settings.refresh > 0 ? 1.0f / float(s.settings.refresh) : 1.0f / 90.0f;
    const float want = st.cine3d && s_cinematic.load() ? 1.0f : 0.0f;
    s_cineRamp += clampf(want - s_cineRamp, -dt / 0.5f, dt / 0.5f);
    const float strength = clampf(st.stereoStrength * (1.0f + 0.6f * s_cineRamp), 0.0f, 0.10f);
    const float focus = clampf(st.stereoFocus, 1.0f, 2500.0f);  // the same plane: more depth behind it
    const float on = st.stereo ? 1.0f : 0.0f;
    if (on == s_stereoApplied[0] && std::fabs(strength - s_stereoApplied[1]) < 1e-5f &&
        std::fabs(focus - s_stereoApplied[2]) < 0.5f)
        return;
    s_stereoApplied[0] = on;
    s_stereoApplied[1] = strength;
    s_stereoApplied[2] = focus;
    aurora_xr_set_stereo(st.stereo, strength, focus);
}

void runFrame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    s.frame = {XR_TYPE_FRAME_STATE};
    if (!ok(xrWaitFrame(s.session, &wi, &s.frame), "xrWaitFrame")) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (!ok(xrBeginFrame(s.session, &bi), "xrBeginFrame")) return;
    readSettings(false);
    if (s_refreshDirty) {
        s_refreshDirty = false;
        setRefreshRate();
    }
    updatePerformance();
    updateScreenStereo();
    locateHead();
    updatePad();
    updateTriggers();
    updateRumble();
    const bool screen = s.frame.shouldRender && copyNewestFrame();

    const XrCompositionLayerBaseHeader* layers[12];
    uint32_t layerCount = 0;
    XrCompositionLayerQuad pointerQuads[2], panelQuad, labelQuad;
    s.hudShown = s.dialogueShown = false;
    // the options menu opens 1 m in front of the player, at eye height, facing them (the diorama)
    const bool menuOpen = s_menuOpen.load();
    if (menuOpen && !s.menuWasOpen) {
        WqXrHead head{};
        {
            std::lock_guard lock{s_shared};
            head = s_heads[(s_nextTag - 1) % kHeadHistory].head;
        }
        const XrQuaternionf q{head.rot[0], head.rot[1], head.rot[2], head.rot[3]};
        XrVector3f f = qrot(q, {0, 0, -1});
        f.y = 0.0f;
        f = vnorm(f);
        const float yaw = std::atan2(-f.x, -f.z);
        s.menuPose.orientation = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
        s.menuPose.position = {head.pos[0] + f.x, head.pos[1] - 0.1f, head.pos[2] + f.z};
        s.mouseX = s.mouseY = -1.0f;
        xrlog("options menu: the panel in front of the player, the laser pointer on");
    }
    s.menuWasOpen = menuOpen;
    XrCompositionLayerPassthroughFB room{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    if (s.frame.shouldRender && updatePassthrough()) {  // the room first, the screen over it
        room.layerHandle = s.passthroughLayer;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&room;
    }
    // The screen: one quad for both eyes, or in 3D one per eye over its half of the picture, at the
    // same place, so each eye sees its own picture of the scene.
    XrCompositionLayerQuad quads[2];
    // The diorama: each eye's half as drawn, for the eye where it was when the frame was drawn, with
    // the frustum it was drawn with; the compositor reprojects to where the eye is now.
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                     {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    HeadSnap drawnFor;
    XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (screen && s.dioramaFrame && s.sideBySide && headForTag(s.frameTag, drawnFor)) {
        // left | right | HUD (Aurora patch 0026), or left | right
        const int32_t eyeW = int32_t(s.screenW / (s.hudFrame ? 3 : 2));
        const XrFovf fov{std::atan(drawnFor.head.tanLeft), std::atan(drawnFor.head.tanRight),
                         std::atan(drawnFor.head.tanUp), std::atan(drawnFor.head.tanDown)};
        for (int eye = 0; eye < 2; eye++) {
            projViews[eye].pose = drawnFor.eyes[eye];
            projViews[eye].fov = fov;
            projViews[eye].subImage.swapchain = s.swapchain;
            // a flat frame (a menu open) has the left eye's picture only, for both
            const int src = s.stereoFrame ? eye : 0;
            projViews[eye].subImage.imageRect = {{src * eyeW, 0}, {eyeW, int32_t(s.screenH)}};
        }
        proj.space = s.space;
        proj.viewCount = 2;
        proj.views = projViews;
        // the eyes carry the table's mask in their alpha: the room shows around the model
        if (s.hudFrame) proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
        if (s.hudFrame) {
            const Settings& st = s.settings;
            const float r = st.sphere ? 0.5f * st.tableWidth : 0.5f * st.tableDepth;
            // the model's middle (the bubble's is 0.35 radii above Link, pc_vr_diorama.cpp)
            const float midY = st.sphere ? -st.tableHeight + 0.35f * r : -st.tableHeight + 0.1f;
            hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            hud.space = s.space;
            hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hud.subImage.swapchain = s.swapchain;
            hud.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            const XrRect2Di full{{2 * eyeW, 0}, {eyeW, int32_t(s.screenH)}};
            const bool panelOpen = wqp::isOpen();
            if (s_menuOpen.load()) {
                // the options menu: the panel floating in front of the player, where it opened
                const float w = 1.3f, h = w * float(s.screenH) / float(eyeW);
                hud.subImage.imageRect = full;
                hud.pose = s.menuPose;
                hud.size = {w, h};
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hud;
                const int pl = updatePointer(hud.pose, hud.size, pointerQuads);
                for (int i = 0; i < pl; i++) layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&pointerQuads[i];
            } else if (st.window) {
                // the window: a dialogue's message box (the bottom of the 2D, as the diorama's) on a
                // panel just above it, or the HUD on a panel just below it, as wide as the window and
                // turned with it. Neither is taken by a grip (a grip takes the window).
                const float hh = 0.5f * st.windowWidth * 9.0f / 16.0f;
                hud.pose.orientation = windowPose(st).orientation;
                if (st.windowHudOn) {
                    // window_hud_on: the HUD, dialogues and all, over the window itself, as on a TV: its
                    // rectangle a centimetre in front, moving and scaling with it
                    hud.subImage.imageRect = full;
                    hud.pose.position = onWindow(st, 0.0f, 0.0f, 0.01f);
                    hud.size = {st.windowWidth, 2.0f * hh};
                } else if (s_dialogue.load()) {
                    const int32_t x0 = int32_t(eyeW * 0.08f), y0 = int32_t(s.screenH * 0.60f);
                    const int32_t cw = int32_t(eyeW * 0.84f), ch = int32_t(s.screenH * 0.38f);
                    hud.subImage.imageRect = {{2 * eyeW + x0, y0}, {cw, ch}};
                    const float w = 0.8f * st.windowWidth, h = w * float(ch) / float(cw);
                    hud.pose.position = onWindow(st, 0.0f, hh + 0.03f + 0.5f * h, 0.0f);
                    hud.size = {w, h};
                } else {
                    const float w = st.windowWidth, h = w * float(s.screenH) / float(eyeW);
                    hud.subImage.imageRect = full;
                    hud.pose.position = onWindow(st, 0.0f, -hh - 0.02f - 0.5f * h, 0.0f);
                    hud.size = {w, h};
                }
                if (!panelOpen) layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hud;
            } else if (s_dialogue.load()) {
                // a dialogue: the message box (the bottom of the 2D) on its own panel above the model,
                // `dialogue_back` behind its middle (larger with the distance, the text the same size to
                // the eye), where a grip moved it (dialogue_offset), facing the player
                const int32_t x0 = int32_t(eyeW * 0.08f), y0 = int32_t(s.screenH * 0.60f);
                const int32_t cw = int32_t(eyeW * 0.84f), ch = int32_t(s.screenH * 0.38f);
                hud.subImage.imageRect = {{2 * eyeW + x0, y0}, {cw, ch}};
                const float w = 1.1f * (st.tableDistance + st.dialogueBack) / std::max(0.3f, st.tableDistance);
                const float h = w * float(ch) / float(cw);
                // above the model, but never higher than the eyes (a large bubble's top can be far above)
                const float aboveY = std::min(midY + r + 0.08f + 0.5f * h, latestHeadPos().y - 0.05f);
                hud.pose.position = {st.tableX + st.dialogueOffset[0], aboveY + st.dialogueOffset[1],
                                     -(st.tableDistance + st.dialogueBack) + st.dialogueOffset[2]};
                hud.pose.orientation = facingPlayer(hud.pose.position);
                hud.size = {w, h};
                if (!panelOpen) layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hud;
                s.dialogueShown = !panelOpen;
                s.dialoguePanel = hud.pose;
                s.dialogueSize = hud.size;
            } else {
                // the HUD: a large panel fixed in the room (hud_pose), where a grip on it put it
                const float w = st.hudWidth, h = w * float(s.screenH) / float(eyeW);
                hud.subImage.imageRect = full;
                hud.pose.position = {st.hudPose[0], st.hudPose[1], st.hudPose[2]};
                const float yaw = st.hudPose[3] * 0.0174532925f;
                hud.pose.orientation = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
                hud.size = {w, h};
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hud;
                s.hudShown = !panelOpen;
                s.hudPanel = hud.pose;
                s.hudSize = hud.size;
            }
        }
    } else if (screen) {
        const Settings& st = s.settings;
        const int32_t eyeW = int32_t(s.sideBySide ? s.screenW / 2 : s.screenW);
        const int eyes = s.stereoFrame ? 2 : 1;
        for (int eye = 0; eye < eyes; eye++) {
            XrCompositionLayerQuad& quad = quads[eye];
            quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.space = s.space;
            quad.eyeVisibility = eyes == 1 ? XR_EYE_VISIBILITY_BOTH
                                 : eye == 0 ? XR_EYE_VISIBILITY_LEFT
                                            : XR_EYE_VISIBILITY_RIGHT;
            quad.subImage.swapchain = s.swapchain;
            quad.subImage.imageRect = {{eye * eyeW, 0}, {eyeW, int32_t(s.screenH)}};
            quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            quad.pose.position = {0.0f, st.height, -st.distance};
            quad.size = {st.width, st.width * float(s.screenH) / float(eyeW)};
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&quad;
        }
        // the options menu on the screen: the laser pointer at it
        if (menuOpen && !wqp::isOpen()) {
            const int pl = updatePointer(quads[0].pose, quads[0].size, pointerQuads);
            for (int i = 0; i < pl; i++) layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&pointerQuads[i];
        }
    }
    // the settings panel over everything, in either mode, with the laser pointer; or, in the diorama,
    // a faint ray where a hand points at the HUD or the dialogues (a grip there takes them)
    if (wqp::isOpen()) {
        const int pl = updatePanelPointer(pointerQuads);
        if (wqp::layer(s.space, &panelQuad)) layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&panelQuad;
        for (int i = 0; i < pl; i++) layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&pointerQuads[i];
    } else if (!menuOpen && s.settings.diorama && (s.hudShown || s.dialogueShown)) {
        const int pl = updatePanelRays(pointerQuads);
        for (int i = 0; i < pl; i++) layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&pointerQuads[i];
    }
    // the model's (or the window's) size while both hands scale it, above it
    if ((s.settings.diorama || s.settings.window) && SDL_GetTicks() < s.sizeShownUntil) {
        const Settings& st = s.settings;
        const float r = st.sphere ? 0.5f * st.tableWidth : 0.5f * st.tableDepth;
        const float midY = st.sphere ? -st.tableHeight + 0.35f * r : -st.tableHeight + 0.1f;
        XrPosef at;
        at.position = st.window ? onWindow(st, 0.0f, 0.5f * st.windowWidth * 9.0f / 16.0f + 0.05f, 0.0f)
                                : XrVector3f{st.tableX, midY + r + 0.05f, -st.tableDistance};
        at.orientation = facingPlayer(at.position);
        char text[32];
        snprintf(text, sizeof text, "%.2f m", st.window ? st.windowWidth : st.tableWidth);
        for (char* c = text; *c; c++)
            if (*c == '.') *c = ',';
        if (wqp::label(text, at, s.space, &labelQuad)) layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&labelQuad;
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = s.frame.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    ok(xrEndFrame(s.session, &ei), "xrEndFrame");
}

void threadMain() {
    JNIEnv* env = nullptr;
    if (s.vm) s.vm->AttachCurrentThread(&env, nullptr);
    if (!initEgl() || !initSession() || !initActions() || !initCopy()) {
        xrlog("headset layer failed; the game stays in its window");
        aurora_xr_present_enable(false);
        return;
    }
    wqp::init({hostGet, hostSet, hostResetPosition, hostResetPanels}, s.session, s.srgb);
    s.panelReady = true;
    xrlog("headset layer ready; waiting for the session");
    while (!s.exiting) {
        pollEvents();
        if (!s.running) {
            updatePad();  // attach the gamepad early, released while not focused
            usleep(10000);
            continue;
        }
        runFrame();
    }
    xrlog("session over");
    if (s.vm) s.vm->DetachCurrentThread();
}

}  // namespace

extern "C" void wq_xr_set(const char* key, const char* value) {
    static std::mutex fileMutex;
    std::lock_guard lock{fileMutex};
    if (s.settingsPath[0] == '\0' || key == nullptr || value == nullptr) return;
    std::vector<std::string> lines;
    if (FILE* f = fopen(s.settingsPath, "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) lines.emplace_back(line);
        fclose(f);
    }
    const size_t keyLen = strlen(key);
    bool replaced = false;
    for (std::string& l : lines) {
        const char* p = l.c_str();
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, keyLen) != 0) continue;
        const char* q = p + keyLen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        l = std::string(key) + " = " + value + "\n";
        replaced = true;
    }
    if (!replaced) lines.push_back(std::string(key) + " = " + value + "\n");
    if (FILE* f = fopen(s.settingsPath, "w")) {
        for (const std::string& l : lines) fputs(l.c_str(), f);
        fclose(f);
    }
    s_settingsDirty = true;
    xrlog("settings: %s = %s (options menu)", key, value);
}

extern "C" void wq_xr_set_camera_heading(float heading, bool rotate, bool dialogue) {
    s_camHeading.store(heading);
    s_camHeadingValid.store(true);
    s_rotateStick.store(rotate);
    s_dialogue.store(dialogue);
}

extern "C" void wq_xr_set_cinematic(bool cinematic) { s_cinematic.store(cinematic); }

extern "C" void wq_xr_menu_state(bool open, float width, float height, bool hovered) {
    s_menuOpen.store(open);
    s_menuW.store(width);
    s_menuH.store(height);
    s_menuHovered.store(hovered);
}

extern "C" bool wq_xr_head(WqXrHead* out) {
    std::lock_guard lock{s_shared};
    if (!s_sharedRunning) return false;
    const HeadSnap& latest = s_heads[(s_nextTag - 1) % kHeadHistory];
    if (latest.head.tag == 0) return false;
    *out = latest.head;
    return true;
}

extern "C" void wq_xr_table(WqXrTable* out) {
    std::lock_guard lock{s_shared};
    const Settings& st = s_sharedSettings;
    out->diorama = st.diorama && s_sharedRunning;
    out->sphere = st.sphere;
    out->width = st.tableWidth;
    out->depth = st.tableDepth;
    out->span = st.tableSpan;
    out->distance = st.tableDistance;
    out->height = st.tableHeight;
    out->fade = st.tableFade;
    out->followYaw = st.tableFollowYaw;
    out->x = st.tableX;
    out->yaw = st.tableYaw * 0.0174532925f;
    out->autoScreen = st.autoScreen;
    out->cineScreen = st.cineScreen;
    out->birdsFree = st.birdsFree;
    out->birdRange = st.birdRange;
    out->window = st.window && s_sharedRunning;
    out->windowWidth = st.windowWidth;
    out->windowHeight = st.windowWidth * 9.0f / 16.0f;
    out->windowX = st.windowX;
    out->windowY = st.windowHeight;
    out->windowDistance = st.windowDistance;
    out->windowYaw = st.windowYaw * 0.0174532925f;
    out->windowScale = st.windowScale;
    out->windowDepth = st.windowDepth;
    out->windowPopout = st.windowPopoutOn ? st.windowPopout : 0.0f;
    out->windowFade = st.windowFade;
}

extern "C" bool wq_xr_start(void) {
    const char* e = getenv("WQ_XR");
    if (e && !strcmp(e, "0")) {
        xrlog("off (WQ_XR=0)");
        return false;
    }
    if (const char* dir = SDL_GetAndroidExternalStoragePath()) {
        snprintf(s.settingsPath, sizeof s.settingsPath, "%s/user", dir);
        mkdir(s.settingsPath, 0755);
        snprintf(s.settingsPath, sizeof s.settingsPath, "%s/user/xr.ini", dir);
    }
    if (!initInstance()) {
        xrlog("no OpenXR headset; the game stays in its window");
        if (s.instance) xrDestroyInstance(s.instance);
        s.instance = XR_NULL_HANDLE;
        return false;
    }
    readSettings(true);
    // The internal resolution in the headset (pc_main.cpp applyFrameBufferScale reads COS_FB_SCALE
    // after aurora_initialize), unless the player chose one (the options menu's settings.ini, or the
    // environment): the headset's window is large, and stereo draws every frame twice.
    if (s.settings.resolution > 0 && getenv("COS_FB_SCALE") == nullptr) {
        char scale[32];
        snprintf(scale, sizeof scale, "%.4f", s.settings.resolution / 480.0);
        setenv("COS_FB_SCALE", scale, 1);
        xrlog("internal resolution: %d lines (COS_FB_SCALE=%s)", s.settings.resolution, scale);
    }
    // The Touch controllers also appear to Android as a gamepad (vendor 0x2833, product 0x0160) that
    // sends nothing useful in VR; the virtual gamepad stands in for them.
    SDL_SetHint(SDL_HINT_JOYSTICK_BLACKLIST_DEVICES, "0x2833/0x0160");
    std::thread(threadMain).detach();
    xrlog("headset layer started");
    return true;
}
