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
    int refresh = 120;       // Hz, the headset's nearest at or below; 120 shows 30 and 60 fps evenly
    // stereoscopic 3D (Aurora patch 0024): each eye its own picture of the scene
    bool stereo = true;
    float stereoStrength = 0.012f;  // how far the distance moves per eye, in half picture widths
    float stereoFocus = 600.0f;     // the depth (game units) that stays on the screen
    float hudWidth = 0.9f;          // the diorama's HUD panel, metres across
    // the game's internal height in pixels in the headset (COS_FB_SCALE = resolution / 480), read at
    // start; 0 = the headset window's own (4128x2208, which drawn twice per frame is too much)
    int resolution = 1200;
    // the diorama (mode = maqueta, pc_vr_diorama.cpp): the world as a model in the room, in a bubble
    // (a sphere, as GalaxyQuest's) or on a table (a box)
    bool diorama = false;
    bool sphere = true;
    float tableWidth = 1.0f, tableDepth = 0.7f;  // m: the bubble's diameter, or the table's width and depth
    float tableSpan = 30.0f;                     // m of the game's world across it
    float tableDistance = 1.0f;                  // m, its middle in front of the eyes
    float tableHeight = 0.3f;                    // m, Link (the table's top) below the eyes
    float tableFade = 0.2f;                      // m, the edges' fade
    bool tableFollowYaw = true;                  // the world turns with the game camera
    float tableX = 0.0f;                         // m, to the right of straight ahead (the grip moves it)
    float tableYaw = 0.0f;                       // degrees, the world turned by the grip
};

const char kDefaultSettings[] =
    "# WakerQuest: the screen in the headset. Read again while playing.\n"
    "# distance and width in metres, height in metres above the eyes,\n"
    "# passthrough 1 = the room around the screen, 0 = black; refresh in Hz (72, 80, 90, 120).\n"
    "distance = 5.0\n"
    "width = 4.5\n"
    "height = 0.0\n"
    "passthrough = 1\n"
    "refresh = 120\n"
    "# 3D: stereo 1/0; stereo_strength = depth of what is far (0.004-0.03),\n"
    "# stereo_focus = the distance in the game that stays on the screen (100-3000).\n"
    "stereo = 1\n"
    "stereo_strength = 0.012\n"
    "stereo_focus = 600\n"
    "# the game's internal height in pixels (720, 900, 1080, 1200, 1440); read when the game starts.\n"
    "resolution = 1200\n"
    "# mode = pantalla (a giant screen) or maqueta (the world as a model in the room).\n"
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
    "bubble_fade = 0.2\n"
    "table_depth = 70\n"
    "table_follow_yaw = 1\n";

void applySetting(Settings& s, const char* key, const char* v) {
    const float f = (float)atof(v);
    const bool on = !(!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "false"));
    if (!strcmp(key, "distance")) s.distance = clampf(f, 0.5f, 30.0f);
    else if (!strcmp(key, "width")) s.width = clampf(f, 0.5f, 40.0f);
    else if (!strcmp(key, "height")) s.height = clampf(f, -10.0f, 10.0f);
    else if (!strcmp(key, "passthrough")) s.passthrough = on;
    else if (!strcmp(key, "refresh")) s.refresh = int(f);
    else if (!strcmp(key, "stereo")) s.stereo = on;
    else if (!strcmp(key, "stereo_strength")) s.stereoStrength = clampf(f, 0.0f, 0.1f);
    else if (!strcmp(key, "stereo_focus")) s.stereoFocus = clampf(f, 1.0f, 16000.0f);
    else if (!strcmp(key, "resolution")) s.resolution = f <= 0.0f ? 0 : int(clampf(f, 480.0f, 2400.0f));
    else if (!strcmp(key, "mode")) s.diorama = !strcmp(v, "maqueta") || !strcmp(v, "diorama") || !strcmp(v, "table");
    else if (!strcmp(key, "shape")) s.sphere = strcmp(v, "caja") != 0 && strcmp(v, "box") != 0 && strcmp(v, "table") != 0;
    else if (!strcmp(key, "bubble_size")) s.tableWidth = clampf(f, 0.2f, 4.0f);
    else if (!strcmp(key, "bubble_span") || !strcmp(key, "table_span")) s.tableSpan = clampf(f, 2.0f, 500.0f);
    else if (!strcmp(key, "bubble_distance") || !strcmp(key, "table_distance")) s.tableDistance = clampf(f, 0.2f, 5.0f);
    else if (!strcmp(key, "bubble_height") || !strcmp(key, "table_height")) s.tableHeight = clampf(f, -1.0f, 2.0f);
    else if (!strcmp(key, "bubble_fade") || !strcmp(key, "table_fade")) s.tableFade = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "bubble_x")) s.tableX = clampf(f, -5.0f, 5.0f);
    else if (!strcmp(key, "bubble_yaw")) s.tableYaw = f;
    else if (!strcmp(key, "table_width")) s.tableWidth = clampf(f, 20.0f, 400.0f) / 100.0f;
    else if (!strcmp(key, "table_depth")) s.tableDepth = clampf(f, 20.0f, 400.0f) / 100.0f;
    else if (!strcmp(key, "table_follow_yaw")) s.tableFollowYaw = on;
    else if (!strcmp(key, "hud_width")) s.hudWidth = clampf(f, 0.2f, 3.0f);
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
    bool dragging = false;
    XrVector3f dragHand{};
    float dragHandYaw = 0.0f;
    float dragX = 0.0f, dragHeight = 0.0f, dragDistance = 0.0f, dragYaw = 0.0f;
    bool clickWas = false;
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
    s.settings = next;
    {
        std::lock_guard lock{s_shared};
        s_sharedSettings = next;
    }
    // the screen's 3D; the diorama sets its own eye shift every frame (pc_vr_diorama.cpp)
    if (!next.diorama) aurora_xr_set_stereo(next.stereo, next.stereoStrength, next.stereoFocus);
    xrlog("settings: mode %s; table %.0fx%.0f cm, %.0f m of the game across, %.2f m away, %.2f m below the eyes, "
          "fade %.2f m, %s the camera",
          next.diorama ? "maqueta" : "pantalla", next.tableWidth * 100.0f, next.tableDepth * 100.0f, next.tableSpan,
          next.tableDistance, next.tableHeight, next.tableFade, next.tableFollowYaw ? "turning with" : "not turning with");
    xrlog("settings: screen %.1f m wide, %.1f m away, %.1f m up; passthrough %s; refresh %d Hz; 3D %s "
          "(strength %.3f, focus %.0f)",
          next.width, next.distance, next.height, next.passthrough ? "on" : "off", next.refresh,
          next.stereo ? "on" : "off", next.stereoStrength, next.stereoFocus);
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
    o_color = u_alpha != 0 ? vec4(c * t.a, t.a) : vec4(c, 1.0);
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
    s.stickClick = makeAction("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, false);

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
        {s.stickClick, "/user/hand/right/input/thumbstick/click"},
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

// The room as the bottom layer, started and paused as the setting changes. True while it shows.
bool updatePassthrough() {
    // the diorama sits in the room
    const bool want = (s.settings.passthrough || s.settings.diorama) && s.hasPassthrough;
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

// The diorama's grip (the right grip held): the model follows the hand, moved and turned about the
// vertical; saved to xr.ini on release; the right stick's click while holding puts it back.
void updateDrag(bool grip, bool click) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (!s.handSpace || XR_FAILED(xrLocateSpace(s.handSpace, s.space, s.frame.predictedDisplayTime, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        return;
    }
    const XrQuaternionf& q = loc.pose.orientation;
    // the hand's forward (-z) on the ground: its heading
    const float fx = -2.0f * (q.x * q.z + q.w * q.y), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float handYaw = std::atan2(fx, -fz);
    Settings& st = s.settings;
    if (grip && !s.dragging) {
        s.dragging = true;
        s.dragHand = loc.pose.position;
        s.dragHandYaw = handYaw;
        s.dragX = st.tableX;
        s.dragHeight = st.tableHeight;
        s.dragDistance = st.tableDistance;
        s.dragYaw = st.tableYaw;
        xrlog("grip: moving the model");
    }
    if (grip) {
        if (click && !s.clickWas) {
            st.tableX = 0.0f;
            st.tableHeight = 0.3f;
            st.tableDistance = 1.0f;
            st.tableYaw = 0.0f;
            s.dragging = false;  // starts again from here
            xrlog("grip: the model back in its place");
        } else {
            const XrVector3f& p = loc.pose.position;
            st.tableX = s.dragX + (p.x - s.dragHand.x);
            st.tableHeight = s.dragHeight - (p.y - s.dragHand.y);
            st.tableDistance = std::max(0.2f, s.dragDistance - (p.z - s.dragHand.z));
            float dy = handYaw - s.dragHandYaw;
            while (dy > 3.14159265f) dy -= 6.28318531f;
            while (dy < -3.14159265f) dy += 6.28318531f;
            st.tableYaw = s.dragYaw - dy * 57.2957795f;
        }
        publishSettings();
    } else if (s.dragging) {
        s.dragging = false;
        char v[32];
        snprintf(v, sizeof v, "%.3f", st.tableX);
        wq_xr_set("bubble_x", v);
        snprintf(v, sizeof v, "%.3f", st.tableHeight);
        wq_xr_set("bubble_height", v);
        snprintf(v, sizeof v, "%.3f", st.tableDistance);
        wq_xr_set("bubble_distance", v);
        snprintf(v, sizeof v, "%.1f", st.tableYaw);
        wq_xr_set("bubble_yaw", v);
    }
    s.clickWas = click;
}

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
            // the diorama: the right grip moves the model (not the game's Z), except with both
            // triggers held (both triggers + grip open the options menu)
            if (s.settings.diorama && !(l > 0.5f && r > 0.5f)) {
                updateDrag(z > 0.6f, getBool(s.stickClick));
                z = 0.0f;
            }
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

void runFrame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    s.frame = {XR_TYPE_FRAME_STATE};
    if (!ok(xrWaitFrame(s.session, &wi, &s.frame), "xrWaitFrame")) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (!ok(xrBeginFrame(s.session, &bi), "xrBeginFrame")) return;
    readSettings(false);
    locateHead();
    updatePad();
    updateRumble();
    const bool screen = s.frame.shouldRender && copyNewestFrame();

    const XrCompositionLayerBaseHeader* layers[4];
    uint32_t layerCount = 0;
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
            // the HUD's panel: over the table's far edge, a little above it, facing the player
            const Settings& st = s.settings;
            const float w = st.hudWidth;
            const float h = w * float(s.screenH) / float(eyeW);
            hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            hud.space = s.space;
            hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hud.subImage.swapchain = s.swapchain;
            hud.subImage.imageRect = {{2 * eyeW, 0}, {eyeW, int32_t(s.screenH)}};
            const float tilt = -0.2f;  // leaning back a little, towards the eyes above it
            hud.pose.orientation = {std::sin(tilt / 2), 0.0f, 0.0f, std::cos(tilt / 2)};
            if (st.sphere) {
                // over the bubble's far side (its middle is 0.35 radii above Link, pc_vr_diorama.cpp)
                const float r = 0.5f * st.tableWidth;
                hud.pose.position = {st.tableX, -st.tableHeight + 0.35f * r + 0.7f * r + 0.5f * h,
                                     -(st.tableDistance + 0.6f * r)};
            } else {
                hud.pose.position = {st.tableX, -st.tableHeight + 0.1f + 0.5f * h,
                                     -(st.tableDistance + 0.5f * st.tableDepth)};
            }
            hud.size = {w, h};
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hud;
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
