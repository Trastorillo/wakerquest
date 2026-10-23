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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
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
    // the game's internal height in pixels in the headset (COS_FB_SCALE = resolution / 480), read at
    // start; 0 = the headset window's own (4128x2208, which drawn twice per frame is too much)
    int resolution = 1080;
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
    "# the game's internal height in pixels (720, 900, 1080, 1440...); read when the game starts.\n"
    "resolution = 1080\n";

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
    XrPath hands[2] = {XR_NULL_PATH, XR_NULL_PATH};
    SDL_JoystickID padId = 0;
    SDL_Joystick* pad = nullptr;
    int rumbleNow = 0;

    Settings settings;
    char settingsPath[600] = "";
    time_t settingsTime = 0;
    time_t settingsChecked = 0;
};
Xr s;

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

void readSettings(bool force) {
    const time_t now = time(nullptr);
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
    aurora_xr_set_stereo(next.stereo, next.stereoStrength, next.stereoFocus);
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
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec3 c = texture(u_tex, v_uv).rgb;
    if (u_linear != 0)
        c = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
    o_color = vec4(c, 1.0);
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
    const bool want = s.settings.passthrough && s.hasPassthrough;
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
        s.sideBySide = f.sideBySide;
        s.stereoFrame = f.stereo;
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
    updatePad();
    updateRumble();
    const bool screen = s.frame.shouldRender && copyNewestFrame();

    const XrCompositionLayerBaseHeader* layers[3];
    uint32_t layerCount = 0;
    XrCompositionLayerPassthroughFB room{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    if (s.frame.shouldRender && updatePassthrough()) {  // the room first, the screen over it
        room.layerHandle = s.passthroughLayer;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&room;
    }
    // The screen: one quad for both eyes, or in 3D one per eye over its half of the picture, at the
    // same place, so each eye sees its own picture of the scene.
    XrCompositionLayerQuad quads[2];
    if (screen) {
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
