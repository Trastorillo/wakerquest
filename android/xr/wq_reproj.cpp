// Reprojection with depth (wq_reproj.h): the eyes' projection layer of the window, the diorama and the immersive
// mode drawn again at every headset frame for where the eyes are now.
//
// The depth's convention (Aurora patch 0042, include/aurora/xr.h AuroraXrFrame.depth): RGBA8, both eyes side by
// side (left | right), each half of the eye's picture a side, row 0 the picture's top; per texel
//   v = (R * 255 * 256 + G * 255) / 65535   and   1 / distance = v / AURORA_XR_DEPTH_REF
// with distance along the eye's view axis (the eye's -z) in metres of the room; v 0 = nothing drawn (infinitely
// far). That inverse distance is exactly the w of the point in the eye's space scaled by its distance,
//   (tanX, tanY, -1, 1 / distance) ~ (tanX * distance, tanY * distance, -distance, 1),
// so a texel with no data (w 0) is a direction: it only turns with the eye. The grid's vertex shader takes that
// homogeneous point straight through clipFromSource = projection(now) * view(now) * pose(drawn), one matrix per eye
// computed here: no per-pixel work besides sampling the colour.
//
// Lifetimes: the depth's AHardwareBuffer is Aurora's (its slot's, as the colour's); copyNewestFrame takes its own
// reference with the frame's (acquire when taken, release once the copy is submitted) and imports it here once
// (an EGLImage holding one more reference, dropped with the oldest of six, as wq_xr.cpp's textureFor, or when
// reprojection is turned off). Its sync file is waited for (on the GPU) with the colour's before the copy reads it,
// and the copy's fence (wq_xr.cpp pendingRead) covers these reads too before Aurora gets the slot back.
#include "wq_reproj.h"

#include <aurora/xr.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace wqr {
namespace {

void rlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void rlog(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[xr] reprojection: %s\n", line);
    fflush(stderr);
}

uint64_t nowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

constexpr int kGridX = 160, kGridY = 90;  // the grid's vertices per eye
constexpr float kNear = 0.01f;            // m: nearer than this is clipped
constexpr int kQueries = 4;               // GPU timer queries in flight
constexpr float kWiden = 1.1f;            // mode 2: the drawn frustum's tangents widened by this about its middle
constexpr uint64_t kLogEveryNs = 5000000000ull;

struct Imported {  // an Aurora depth buffer as a GL texture (and a framebuffer to blit from)
    AHardwareBuffer* buffer = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    GLuint fbo = 0;
};

struct State {
    Egl egl{};
    bool ready = false;
    int mode = 0;
    // the newest frame's eyes: colour (as the screen's swapchain gets it: premultiplied, sRGB-encoded in an sRGB
    // texture when the swapchains are) and depth (Aurora's, as it is)
    GLuint colorTex = 0, colorFbo = 0;
    uint32_t eyeW = 0, eyeH = 0;
    bool colorSrgb = false;
    GLuint depthTex = 0, depthFbo = 0;
    uint32_t depthW = 0, depthH = 0;
    std::vector<Imported> imports;
    bool pendingColor = false, pendingDepth = false;
    bool valid = false;
    uint32_t tag = 0;
    XrPosef capPose[2]{};   // the captured frame's eyes as it was drawn for them (commit), and its frustum
    XrFovf capFov{};
    bool capWindow = false;  // the captured frame is a window's with its mask: its window's pose and half size
    XrPosef capWindowPose{};
    float capWindowHalf[2] = {0.0f, 0.0f};
    uint32_t importFails = 0;  // depth buffers that could not be imported (logged at the first, then every 600)
    // the output: one swapchain both eyes wide, a depth buffer for the grid's folds, the grid
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    uint32_t outW = 0, outH = 0;
    bool outSrgb = false;
    GLuint outFbo = 0, depthRb = 0;
    GLuint prog = 0, vao = 0, ibo = 0;
    GLsizei indexCount = 0;
    GLint uClip = -1, uTan = -1, uDepthRect = -1, uEye = -1, uHalfTexel = -1;
    GLint uRefine = -1, uRayA = -1, uRayM = -1;
    GLint uWindow = -1, uWinPlane = -1, uWinO = -1, uWinM = -1, uWinHalf = -1;
    int refine = 0;  // reprojection_refine (0 by default, as xr.ini's): 0 off, 1 on, 2 the unconverged in magenta,
                     // 3 the depth shown over the colour
    bool failed = false;  // the program or the swapchain could not be made: off until turned on again
    // the pass's cost: GPU (EXT_disjoint_timer_query) or the CPU's submission
    bool timer = false;
    GLuint queries[kQueries] = {};
    bool queryBusy[kQueries] = {};
    int queryNext = 0;
    uint64_t gpuNs = 0, cpuNs = 0;
    uint32_t gpuSamples = 0, drawn = 0, fallbacks = 0, captures = 0;
    uint64_t logAt = 0;
};
State g;

// ---- matrices (row-major, points as columns) ---------------------------------------------------------------

struct M4 {
    float m[4][4];
};

M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++) s += double(a.m[i][k]) * double(b.m[k][j]);
            r.m[i][j] = float(s);
        }
    return r;
}

// the pose as a matrix: the space from the pose's own (rotation, then translation)
M4 fromPose(const XrPosef& p) {
    const XrQuaternionf& q = p.orientation;
    M4 r{};
    r.m[0][0] = 1 - 2 * (q.y * q.y + q.z * q.z);
    r.m[0][1] = 2 * (q.x * q.y - q.w * q.z);
    r.m[0][2] = 2 * (q.x * q.z + q.w * q.y);
    r.m[1][0] = 2 * (q.x * q.y + q.w * q.z);
    r.m[1][1] = 1 - 2 * (q.x * q.x + q.z * q.z);
    r.m[1][2] = 2 * (q.y * q.z - q.w * q.x);
    r.m[2][0] = 2 * (q.x * q.z - q.w * q.y);
    r.m[2][1] = 2 * (q.y * q.z + q.w * q.x);
    r.m[2][2] = 1 - 2 * (q.x * q.x + q.y * q.y);
    r.m[0][3] = p.position.x;
    r.m[1][3] = p.position.y;
    r.m[2][3] = p.position.z;
    r.m[3][3] = 1.0f;
    return r;
}

// the view (OpenXR: the inverse of the eye's pose): R^T, -R^T t
M4 viewOf(const XrPosef& p) {
    const M4 a = fromPose(p);
    M4 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i];
    for (int i = 0; i < 3; i++) r.m[i][3] = -(a.m[0][i] * a.m[0][3] + a.m[1][i] * a.m[1][3] + a.m[2][i] * a.m[2][3]);
    r.m[3][3] = 1.0f;
    return r;
}

// the projection from the frustum's tangents, OpenGL ES clip space (x, y, z within -w..w). Its depth row puts a
// point `distance` in front at z_ndc = 1 - 2 kNear / distance: -1 at kNear, 1 at infinity (w 0: a direction),
// monotonic in between, for the depth test of the grid's folds (cleared to 1, GL_LEQUAL)
M4 projection(float l, float r, float d, float u) {
    M4 p{};
    p.m[0][0] = 2.0f / (r - l);
    p.m[0][2] = (r + l) / (r - l);
    p.m[1][1] = 2.0f / (u - d);
    p.m[1][2] = (u + d) / (u - d);
    p.m[2][2] = -1.0f;
    p.m[2][3] = -2.0f * kNear;
    p.m[3][2] = -1.0f;
    return p;
}

// ---- GL -----------------------------------------------------------------------------------------------------

GLuint compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint okc = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &okc);
    if (!okc) {
        char log[1024] = "";
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        rlog("shader: %s", log);
    }
    return sh;
}

// The grid: each vertex (gl_VertexID: column i, row j; v 0 at the picture's bottom) reads its eye's depth texel,
// makes the point in the drawn eye's space as (tanX, tanY, -1, 1 / distance) and takes it through u_clip. The
// colour is sampled where the vertex was in the picture (both eyes side by side in u_color; clamped half a texel
// inside the eye's half, so the other eye never bleeds in).
const char* kVs = R"(#version 300 es
precision highp float;
precision highp int;
uniform mat4 u_clip;        // the eye's clip space now from the drawn eye's space (column-major)
uniform vec4 u_tan;         // the drawn frustum's tangents: left, right, down, up
uniform vec4 u_depthRect;   // the eye's first depth column, its width and height (texels), 1 / AURORA_XR_DEPTH_REF
uniform float u_eye;        // 0 left, 1 right
uniform float u_halfTexel;  // half a colour texel of the eye's width, in [0, 1] of the eye
uniform highp sampler2D u_depth;
uniform highp sampler2D u_color;
// the window (its frames with the mask: the window mode's, a special scene's): its plane in the drawn eye's space,
// n . x = c (n toward the player), as the frame was drawn; u_window 0: not such a frame
uniform int u_window;
uniform vec4 u_winPlane;    // n, c
uniform int u_refine;       // > 0: the cells around a depth edge marked for the fragment shader's refinement
const int kGridX = 160;  // as wq_reproj.cpp's kGridX, kGridY
const int kGridY = 90;
// the grid reaches kOver past the picture on every side (its first and last rows and columns), the edge's depth and
// colour stretched out there, so a moved eye sees no empty strip at the picture's border
const float kOver = 0.02;
out vec2 v_uv;
out vec2 v_g;  // the vertex's place in its eye's picture (0..1, v 0 at the bottom): the refinement's first guess
// the cell this vertex closes (its lower left: the grid's indices make it the provoking vertex of both its
// triangles) has a depth edge: refined there only
flat out int v_refineCell;
float fetchInv(vec2 gc) {  // 1 / distance at gc of this eye, as Aurora drew it (0: nothing drawn)
    ivec2 q = ivec2(int(u_depthRect.x) + clamp(int(gc.x * u_depthRect.y), 0, int(u_depthRect.y) - 1),
                    clamp(int((1.0 - gc.y) * u_depthRect.z), 0, int(u_depthRect.z) - 1));
    vec4 t = texelFetch(u_depth, q, 0);
    return (floor(t.r * 255.0 + 0.5) * 256.0 + floor(t.g * 255.0 + 0.5)) / 65535.0 * u_depthRect.w;
}
void main() {
    int i = gl_VertexID % kGridX;
    int j = gl_VertexID / kGridX;
    vec2 cell = vec2(1.0 / float(kGridX - 1), 1.0 / float(kGridY - 1)) * (1.0 + 2.0 * kOver);
    vec2 g = vec2(float(i), float(j)) * cell - kOver;
    vec2 gc = clamp(g, 0.0, 1.0);
    vec3 dir = vec3(mix(u_tan.x, u_tan.y, g.x), mix(u_tan.z, u_tan.w, g.y), -1.0);
    // 1 / distance (metres); 0: nothing drawn, infinitely far (a direction: only turned)
    float inv = fetchInv(gc);
    v_uv = vec2((u_eye + clamp(g.x, u_halfTexel, 1.0 - u_halfTexel)) * 0.5, gc.y);
    if (u_window != 0) {
        // outside the window's mask (alpha 0) behind or on its plane (the world behind the window's frame,
        // hidden; not the pop-out's faded front, which is in front of it): its depth is the far world's, whose
        // parallax would drag the window's edge along. Taken on the plane instead (where the eye sees the frame):
        // 1 / distance = n . (tanX, tanY, -1) / c
        if (textureLod(u_color, v_uv, 0.0).a < 0.5 && dot(u_winPlane.xyz, dir) - u_winPlane.w * inv <= 0.0)
            inv = max(dot(u_winPlane.xyz, dir) / u_winPlane.w, 0.0);
    }
    // the refinement's cells: 4 x 4 depth samples over the cell this vertex closes (to its lower left); an edge
    // when 1 / distance jumps both by 0.07 / m and by 5% of the nearest (raw depth: the window's plane rule is
    // left out here, it only marks a few more cells at the frame)
    v_refineCell = 0;
    if (u_refine > 0 && i > 0 && j > 0) {
        float lo = 1e30, hi = 0.0;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                float s = fetchInv(clamp(g - cell * (vec2(float(x), float(y)) + 0.5) / 4.0, 0.0, 1.0));
                lo = min(lo, s);
                hi = max(hi, s);
            }
        if (hi - lo > 0.07 && hi - lo > 0.05 * hi) v_refineCell = 1;
    }
    gl_Position = u_clip * vec4(dir, inv);
    v_g = gc;
}
)";
// The refinement (reprojection_refine): the grid's colour uv is only a first guess, wrong inside the cells that
// cross a depth edge (a near mast over the far sea: the edge followed the grid's cells, ~9 px steps). Per
// fragment, the line of sight of this pixel of the eye now, in the drawn eye's space: P(t) = A + t B (A the eye
// now, B its direction through the pixel: u_rayM * (gl_FragCoord.xy, 1)). A few fixed-point steps from the guess:
//   inv = 1 / distance at uv (the depth texel there; 0: infinitely far)
//   the point of the line at that distance along the drawn eye's axis (-z = distance): t = (-1 / inv - A.z) / B.z,
//   whose tangents are  tan = A.xy * inv + B.xy * s,  s = t * inv = (-1 - A.z * inv) / B.z
//   (inv 0: tan = B.xy / -B.z, the direction alone), and its uv in the drawn frustum.
// A step with no solution (s <= 0: the point behind the eye now) keeps the last uv. The edge then lands at the
// depth's resolution (2 px) instead of the grid's.
// Edges have no single fixed point: where the near hides the far there are two consistent ones (the iteration
// keeps the one nearest its guess, maybe the far: the near edge "eaten"), and in the hole the moved eye uncovers
// none (it swings between near and far: stripes, or the near twice). So, deterministically: the nearest and the
// farthest inverse depth met (the guess's included) are kept; after the steps, one step from the nearest, taken
// when consistent (the depth where it lands is about that one: the near wins); else, when the steps did not
// converge (the last move over 1.5 depth texels), one step from the farthest (the hole filled with the
// background); else the steps' end. Only in the cells the vertex shader marked (a depth edge in them).
// reprojection_refine 2 paints the pixels that did not converge magenta; 3 shows the depth over the colour.
const char* kFs = R"(#version 300 es
precision highp float;  // (mediump: the right eye's u, 0.5..1, loses about a texel on Adreno)
precision highp int;
uniform highp sampler2D u_color;
uniform highp sampler2D u_depth;
uniform vec4 u_tan;         // as the vertex shader's
uniform vec4 u_depthRect;
uniform float u_eye;
uniform float u_halfTexel;
uniform int u_refine;       // 0: the grid's uv as it is; 1: refined; 2: refined, the unconverged in magenta;
                            // 3: refined, the depth shown over the colour (grey, near light), to check they match
uniform vec3 u_rayA;        // the eye now, in the drawn eye's space
uniform mat3 u_rayM;        // (gl_FragCoord.xy, 1) -> the pixel's direction in the drawn eye's space
// the window: its plane as drawn (as the vertex shader's), and as the eye sees it now: what is behind the plane
// shows only through its rectangle (the mask, done again for this eye, so the frame's edge stays on the frame):
// the pixel's line of sight in the window's space is O + t D, D = u_winM * (gl_FragCoord.xy, 1); its crossing of
// the plane z 0 within u_winHalf
uniform int u_window;
uniform vec4 u_winPlane;
uniform vec3 u_winO;
uniform mat3 u_winM;
uniform vec2 u_winHalf;
in vec2 v_uv;
in vec2 v_g;
flat in int v_refineCell;
out vec4 o_color;
vec3 tanDir(vec2 g) { return vec3(mix(u_tan.x, u_tan.y, g.x), mix(u_tan.z, u_tan.w, g.y), -1.0); }
vec2 colorUv(vec2 g) { return vec2((u_eye + clamp(g.x, u_halfTexel, 1.0 - u_halfTexel)) * 0.5, g.y); }
// 1 / distance at g of this eye (0: nothing drawn), with alpha the colour's there: in a window's frame, outside its
// mask and not in front of its plane, the plane's (as the vertex shader takes it)
float sceneInv(vec2 g, float alpha) {
    ivec2 px = ivec2(int(u_depthRect.x) + clamp(int(g.x * u_depthRect.y), 0, int(u_depthRect.y) - 1),
                     clamp(int((1.0 - g.y) * u_depthRect.z), 0, int(u_depthRect.z) - 1));
    vec4 t = texelFetch(u_depth, px, 0);
    float inv = (floor(t.r * 255.0 + 0.5) * 256.0 + floor(t.g * 255.0 + 0.5)) / 65535.0 * u_depthRect.w;
    if (u_window != 0 && alpha < 0.5) {
        vec3 dir = tanDir(g);
        if (dot(u_winPlane.xyz, dir) - u_winPlane.w * inv <= 0.0) inv = max(dot(u_winPlane.xyz, dir) / u_winPlane.w, 0.0);
    }
    return inv;
}
float invDepth(vec2 g) {
    return sceneInv(g, u_window != 0 ? textureLod(u_color, colorUv(g), 0.0).a : 1.0);
}
// one step: the uv where the line of sight is at inverse depth inv (ok false: no such point in front of the eye)
vec2 stepFrom(float inv, vec3 b, vec2 g, out bool ok) {
    float s = (-1.0 - u_rayA.z * inv) / b.z;
    ok = s > 0.0;
    if (!ok) return g;
    vec2 tn = u_rayA.xy * inv + b.xy * s;
    return clamp((tn - u_tan.xz) / (u_tan.yw - u_tan.xz), 0.0, 1.0);
}
void main() {
    vec2 uv = v_uv;
    bool unconverged = false;
    vec2 g = v_g;
    if (u_refine > 0 && v_refineCell != 0) {
        vec3 b = u_rayM * vec3(gl_FragCoord.xy, 1.0);
        if (b.z < -1e-6) {
            vec2 prev = g;
            float invMin = 1e30, invMax = -1.0;
            bool ok = true;
            for (int k = 0; k < 3; k++) {
                float inv = invDepth(g);
                invMin = min(invMin, inv);
                invMax = max(invMax, inv);
                vec2 n = stepFrom(inv, b, g, ok);
                if (!ok) break;
                prev = g;
                g = n;
            }
            vec2 texel = 1.0 / u_depthRect.yz;
            bool converged = all(lessThanEqual(abs(g - prev), 1.5 * texel));
            unconverged = !converged;
            // the nearest met, when consistent where it lands
            vec2 gn = stepFrom(invMax, b, g, ok);
            if (ok && abs(invDepth(gn) - invMax) < 0.1 * invMax + 0.002) {
                g = gn;
            } else if (!converged) {
                // the hole: the farthest met (the background)
                vec2 gf = stepFrom(invMin, b, g, ok);
                if (ok) g = gf;
            }
        }
        uv = colorUv(g);
    }
    o_color = texture(u_color, uv);  // as the screen's swapchain has it (premultiplied by its alpha)
    if (u_window != 0) {
        // in front of the plane (what comes out of the window) or behind it, by this pixel's own depth where its
        // colour was taken
        float inv = sceneInv(g, o_color.a);
        if (dot(u_winPlane.xyz, tanDir(g)) - u_winPlane.w * inv <= 0.0) {
            vec3 d = u_winM * vec3(gl_FragCoord.xy, 1.0);
            float t = -u_winO.z / d.z;
            vec2 q = u_winO.xy + t * d.xy;
            if (!(t > 0.0) || abs(q.x) > u_winHalf.x || abs(q.y) > u_winHalf.y) o_color = vec4(0.0);
        }
    }
    if (u_refine == 2 && unconverged) o_color = vec4(1.0, 0.0, 1.0, 1.0);
    if (u_refine == 3 && o_color.a > 0.0) {
        // the depth where the colour was taken, over it (premultiplied, its alpha kept): a misplaced depth shows as
        // a grey shape off its object
        float gray = clamp(sqrt(invDepth(g) * 0.5), 0.0, 1.0);
        o_color.rgb = mix(o_color.rgb, vec3(gray) * o_color.a, 0.6);
    }
}
)";

bool initProgram() {
    GLuint v = compile(GL_VERTEX_SHADER, kVs), f = compile(GL_FRAGMENT_SHADER, kFs);
    g.prog = glCreateProgram();
    glAttachShader(g.prog, v);
    glAttachShader(g.prog, f);
    glLinkProgram(g.prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint linked = 0;
    glGetProgramiv(g.prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = "";
        glGetProgramInfoLog(g.prog, sizeof log, nullptr, log);
        rlog("program did not link: %s", log);
        glDeleteProgram(g.prog);
        g.prog = 0;
        return false;
    }
    glUseProgram(g.prog);
    glUniform1i(glGetUniformLocation(g.prog, "u_color"), 0);
    glUniform1i(glGetUniformLocation(g.prog, "u_depth"), 1);
    g.uClip = glGetUniformLocation(g.prog, "u_clip");
    g.uTan = glGetUniformLocation(g.prog, "u_tan");
    g.uDepthRect = glGetUniformLocation(g.prog, "u_depthRect");
    g.uEye = glGetUniformLocation(g.prog, "u_eye");
    g.uHalfTexel = glGetUniformLocation(g.prog, "u_halfTexel");
    g.uRefine = glGetUniformLocation(g.prog, "u_refine");
    g.uWindow = glGetUniformLocation(g.prog, "u_window");
    g.uWinPlane = glGetUniformLocation(g.prog, "u_winPlane");
    g.uWinO = glGetUniformLocation(g.prog, "u_winO");
    g.uWinM = glGetUniformLocation(g.prog, "u_winM");
    g.uWinHalf = glGetUniformLocation(g.prog, "u_winHalf");
    g.uRayA = glGetUniformLocation(g.prog, "u_rayA");
    g.uRayM = glGetUniformLocation(g.prog, "u_rayM");
    // the grid's triangles (made once): two per cell, 16-bit indices
    std::vector<uint16_t> idx;
    idx.reserve(size_t(kGridX - 1) * (kGridY - 1) * 6);
    for (int j = 0; j + 1 < kGridY; j++)
        for (int i = 0; i + 1 < kGridX; i++) {
            const uint16_t a = uint16_t(j * kGridX + i), b = uint16_t(a + 1), c = uint16_t(a + kGridX), d = uint16_t(c + 1);
            // both triangles end on d (the cell's upper right): GLES's provoking vertex (the last) is then the same
            // for both, the vertex whose flat v_refineCell describes this cell (its lower left)
            idx.insert(idx.end(), {a, b, d, c, a, d});
        }
    g.indexCount = GLsizei(idx.size());
    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);
    glGenBuffers(1, &g.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * sizeof(uint16_t)), idx.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    if (g.timer) glGenQueries(kQueries, g.queries);
    return true;
}

void dropImports() {
    for (Imported& im : g.imports) {
        if (im.fbo) glDeleteFramebuffers(1, &im.fbo);
        if (im.texture) glDeleteTextures(1, &im.texture);
        if (im.image != EGL_NO_IMAGE_KHR) g.egl.destroyImage(g.egl.display, im.image);
        AHardwareBuffer_release(im.buffer);
    }
    g.imports.clear();
}

void dropSwapchain() {
    if (g.chain != XR_NULL_HANDLE) xrDestroySwapchain(g.chain);
    g.chain = XR_NULL_HANDLE;
    g.images.clear();
    g.outW = g.outH = 0;
}

// Everything but the EGL calls: reprojection off (or failed)
void freeAll() {
    dropImports();
    dropSwapchain();
    GLuint fbos[] = {g.colorFbo, g.depthFbo, g.outFbo};
    for (GLuint f : fbos)
        if (f) glDeleteFramebuffers(1, &f);
    GLuint texs[] = {g.colorTex, g.depthTex};
    for (GLuint t : texs)
        if (t) glDeleteTextures(1, &t);
    if (g.depthRb) glDeleteRenderbuffers(1, &g.depthRb);
    if (g.ibo) glDeleteBuffers(1, &g.ibo);
    if (g.vao) glDeleteVertexArrays(1, &g.vao);
    if (g.prog) glDeleteProgram(g.prog);
    if (g.timer && g.queries[0]) glDeleteQueries(kQueries, g.queries);
    for (GLuint& q : g.queries) q = 0;
    for (bool& b : g.queryBusy) b = false;
    g.colorFbo = g.depthFbo = g.outFbo = g.colorTex = g.depthTex = g.depthRb = g.ibo = g.vao = g.prog = 0;
    g.eyeW = g.eyeH = g.depthW = g.depthH = 0;
    g.valid = g.pendingColor = g.pendingDepth = false;
}

// Something of this cannot be made: Aurora stops drawing the depth and all of it is freed; off until reprojection
// is turned off and on again (configure)
void fail(const char* why) {
    rlog("%s: off until turned off and on again", why);
    aurora_xr_set_eye_depth(false);
    freeAll();
    g.failed = true;
}

GLuint makeTexture(GLenum format, uint32_t w, uint32_t h, GLint filter) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, format, GLsizei(w), GLsizei(h));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

const Imported* importDepth(AHardwareBuffer* buffer) {
    for (const Imported& im : g.imports)
        if (im.buffer == buffer) return &im;
    Imported im;
    im.buffer = buffer;
    AHardwareBuffer_acquire(buffer);
    EGLClientBuffer client = g.egl.nativeClientBuffer(buffer);
    const EGLint attr[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    im.image = client ? g.egl.createImage(g.egl.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attr)
                      : EGL_NO_IMAGE_KHR;
    if (im.image == EGL_NO_IMAGE_KHR) {
        if (g.importFails++ % 600 == 0)
            rlog("eglCreateImageKHR for a depth buffer failed (0x%x; %u times)", eglGetError(), g.importFails);
        AHardwareBuffer_release(buffer);
        return nullptr;
    }
    glGenTextures(1, &im.texture);
    glBindTexture(GL_TEXTURE_2D, im.texture);
    g.egl.imageTargetTexture(GL_TEXTURE_2D, (GLeglImageOES)im.image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &im.fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, im.fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, im.texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (g.importFails++ % 600 == 0)
            rlog("a depth buffer cannot be read as a framebuffer (0x%x; %u times)", status, g.importFails);
        glDeleteFramebuffers(1, &im.fbo);
        glDeleteTextures(1, &im.texture);
        g.egl.destroyImage(g.egl.display, im.image);
        AHardwareBuffer_release(buffer);
        return nullptr;
    }
    while (g.imports.size() >= 6) {  // Aurora's three slots, and three more after a size change
        Imported& old = g.imports.front();
        glDeleteFramebuffers(1, &old.fbo);
        glDeleteTextures(1, &old.texture);
        g.egl.destroyImage(g.egl.display, old.image);
        AHardwareBuffer_release(old.buffer);
        g.imports.erase(g.imports.begin());
    }
    g.imports.push_back(im);
    rlog("imported Aurora depth buffer %p as texture %u", (void*)buffer, im.texture);
    return &g.imports.back();
}

bool ensureOutput(XrSession session, uint32_t w, uint32_t h, bool srgb) {
    if (g.chain != XR_NULL_HANDLE && w == g.outW && h == g.outH && srgb == g.outSrgb) return true;
    dropSwapchain();
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sc.format = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    sc.sampleCount = 1;
    sc.width = w;
    sc.height = h;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &sc, &g.chain))) {
        rlog("xrCreateSwapchain %ux%u failed: off", w, h);
        g.chain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g.chain, 0, &n, nullptr);
    g.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(g.chain, n, &n, (XrSwapchainImageBaseHeader*)g.images.data());
    if (!g.outFbo) glGenFramebuffers(1, &g.outFbo);
    if (g.depthRb) glDeleteRenderbuffers(1, &g.depthRb);
    glGenRenderbuffers(1, &g.depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, g.depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, GLsizei(w), GLsizei(h));
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    g.outW = w;
    g.outH = h;
    g.outSrgb = srgb;
    rlog("swapchain %ux%u (both eyes), %u images", w, h, n);
    return true;
}

// the oldest timer query, if done: its time added
void pollQueries() {
    if (!g.timer) return;
    for (int k = 0; k < kQueries; k++) {
        if (!g.queryBusy[k]) continue;
        GLuint avail = 0;
        glGetQueryObjectuiv(g.queries[k], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) continue;
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        GLuint ns = 0;
        glGetQueryObjectuiv(g.queries[k], GL_QUERY_RESULT, &ns);
        g.queryBusy[k] = false;
        if (!disjoint) {
            g.gpuNs += ns;
            g.gpuSamples++;
        }
    }
}

void maybeLog() {
    const uint64_t now = nowNs();
    if (g.logAt == 0) g.logAt = now;
    if (now - g.logAt < kLogEveryNs) return;
    const double secs = double(now - g.logAt) / 1e9;
    g.logAt = now;
    if (g.gpuSamples > 0)
        rlog("mode %d (refine %d), %u headset frames reprojected in %.1f s (%u not: no depth for the frame shown), %u Aurora frames "
             "captured; GPU %.2f ms a pass, CPU %.2f ms",
             g.mode, g.refine, g.drawn, secs, g.fallbacks, g.captures, double(g.gpuNs) / 1e6 / g.gpuSamples,
             g.drawn ? double(g.cpuNs) / 1e6 / g.drawn : 0.0);
    else
        rlog("mode %d (refine %d), %u headset frames reprojected in %.1f s (%u not: no depth for the frame shown), %u Aurora frames "
             "captured; CPU %.2f ms a pass (no GPU timer)",
             g.mode, g.refine, g.drawn, secs, g.fallbacks, g.captures, g.drawn ? double(g.cpuNs) / 1e6 / g.drawn : 0.0);
    g.gpuNs = g.cpuNs = 0;
    g.gpuSamples = g.drawn = g.fallbacks = g.captures = 0;
}

}  // namespace

void init(const Egl& egl) {
    g.egl = egl;
    const char* exts = (const char*)glGetString(GL_EXTENSIONS);
    g.timer = exts != nullptr && strstr(exts, "GL_EXT_disjoint_timer_query") != nullptr;
    g.ready = egl.createImage && egl.destroyImage && egl.nativeClientBuffer && egl.imageTargetTexture;
}

int mode() { return g.mode; }

void configure(XrSession session, int m, int refine) {
    (void)session;
    refine = std::clamp(refine, 0, 3);
    if (refine != g.refine) {
        g.refine = refine;
        if (m > 0)
            rlog("refinement %s", refine == 0 ? "off (the grid's uv)"
                                  : refine == 1 ? "on"
                                  : refine == 2 ? "on, the pixels that did not converge in magenta"
                                                : "on, the depth shown over the colour");
    }
    m = std::clamp(m, 0, 2);
    if (m == g.mode) return;
    const bool wasOn = g.mode > 0;
    g.mode = m;
    if (m > 0 && !g.ready) {
        rlog("no EGL image support: stays off");
        g.mode = 0;
        return;
    }
    if ((m > 0) != wasOn) {
        aurora_xr_set_eye_depth(m > 0);
        if (m == 0) freeAll();
        g.failed = false;
        g.logAt = 0;
        g.gpuNs = g.cpuNs = 0;
        g.gpuSamples = g.drawn = g.fallbacks = g.captures = 0;
    }
    rlog("%s", m == 0 ? "off" : m == 1 ? "on (the eyes' position now; their orientation and frustum as drawn)"
                                       : "on (the eyes' pose and frustum now)");
}

bool beginColor(uint32_t bufferW, uint32_t height, uint32_t eyeW, bool srgb) {
    g.pendingColor = false;
    if (g.mode == 0 || g.failed || eyeW == 0 || height == 0 || bufferW < 2 * eyeW) return false;
    GLint maxDims[2] = {0, 0};
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxDims);
    if (GLint(bufferW) > maxDims[0] || GLint(height) > maxDims[1]) return false;
    if (!g.colorTex || g.eyeW != eyeW || g.eyeH != height || g.colorSrgb != srgb) {
        if (g.colorTex) glDeleteTextures(1, &g.colorTex);
        g.colorTex = makeTexture(srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, 2 * eyeW, height, GL_LINEAR);
        if (!g.colorFbo) glGenFramebuffers(1, &g.colorFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g.colorFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.colorTex, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        g.eyeW = eyeW;
        g.eyeH = height;
        g.colorSrgb = srgb;
        rlog("the eyes' copy: %ux%u (%s)", 2 * eyeW, height, srgb ? "sRGB" : "RGBA8");
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            rlog("the eyes' copy is not a complete framebuffer (0x%x)", status);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            fail("the eyes' copy");
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g.colorFbo);
    // the whole buffer's width: the copy's full-screen triangle lands each buffer texel on one of ours, the
    // eyes in the first 2 * eyeW columns (the rest falls outside and is not drawn)
    glViewport(0, 0, GLsizei(bufferW), GLsizei(height));
    g.pendingColor = true;
    return true;
}

void captureDepth(AHardwareBuffer* depth, uint32_t width, uint32_t height) {
    g.pendingDepth = false;
    if (g.mode == 0 || g.failed || depth == nullptr || width < 2 || height == 0) return;
    const Imported* im = importDepth(depth);
    if (im == nullptr) return;
    if (!g.depthTex || g.depthW != width || g.depthH != height) {
        if (g.depthTex) glDeleteTextures(1, &g.depthTex);
        g.depthTex = makeTexture(GL_RGBA8, width, height, GL_NEAREST);
        if (!g.depthFbo) glGenFramebuffers(1, &g.depthFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.depthFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.depthTex, 0);
        const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        g.depthW = width;
        g.depthH = height;
        rlog("the eyes' depth: %ux%u", width, height);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            rlog("the eyes' depth is not a complete framebuffer (0x%x)", status);
            fail("the eyes' depth");
            return;
        }
    }
    // texel for texel, rows as they are (row 0 the picture's top, as Aurora wrote them)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, im->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.depthFbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, GLint(width), GLint(height), 0, 0, GLint(width), GLint(height), GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    g.pendingDepth = true;
}

void commit(bool captured, uint32_t tag, const XrPosef drawn[2], const XrFovf& drawnFov, const XrPosef* window,
            float windowHalfW, float windowHalfH) {
    g.valid = captured && drawn != nullptr && g.pendingColor && g.pendingDepth;
    g.tag = tag;
    if (g.valid) {
        g.capPose[0] = drawn[0];
        g.capPose[1] = drawn[1];
        g.capFov = drawnFov;
        g.capWindow = window != nullptr;
        if (window != nullptr) {
            g.capWindowPose = *window;
            g.capWindowHalf[0] = windowHalfW;
            g.capWindowHalf[1] = windowHalfH;
        }
    }
    g.pendingColor = g.pendingDepth = false;
    if (g.valid) g.captures++;
}

bool render(XrSession session, XrSpace space, XrTime time, uint32_t tag, bool srgb, const XrPosef* window,
            float windowHalfW, float windowHalfH, XrCompositionLayerProjectionView views[2]) {
    if (g.mode == 0 || g.failed) return false;
    maybeLog();
    if (!g.valid || g.tag != tag || !g.colorTex || !g.depthTex) {
        g.fallbacks++;
        return false;
    }
    // the eyes now
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = time;
    li.space = space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView now[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(session, &li, &vs, 2, &n, now)) || n != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        g.fallbacks++;
        return false;
    }
    if (!g.prog && !initProgram()) {
        fail("the program");
        return false;
    }
    if (!ensureOutput(session, 2 * g.eyeW, g.eyeH, srgb)) {
        fail("the swapchain");
        return false;
    }
    const XrPosef* drawn = g.capPose;
    const XrFovf drawnFov = g.capFov;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(g.chain, &acq, &idx))) return false;
    if (XR_FAILED(xrWaitSwapchainImage(g.chain, &wait))) {
        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g.chain, &rel);
        return false;
    }
    const uint64_t cpuStart = nowNs();  // (after the wait for the compositor's image)
    pollQueries();
    int query = -1;
    if (g.timer && g.queries[0] && !g.queryBusy[g.queryNext]) {
        query = g.queryNext;
        g.queryNext = (g.queryNext + 1) % kQueries;
        glBeginQuery(GL_TIME_ELAPSED_EXT, g.queries[query]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g.outFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.images[idx].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g.depthRb);
    glViewport(0, 0, GLsizei(g.outW), GLsizei(g.outH));
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);  // outside the grid: nothing (transparent)
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(g.prog);
    glBindVertexArray(g.vao);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g.depthTex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.colorTex);
    const float tl = std::tan(drawnFov.angleLeft), tr = std::tan(drawnFov.angleRight);
    const float td = std::tan(drawnFov.angleDown), tu = std::tan(drawnFov.angleUp);
    const uint32_t dEyeW = g.depthW / 2;
    for (int eye = 0; eye < 2; eye++) {
        // where the eye is shown: mode 1, its position now with the orientation and frustum it was drawn with
        // (the compositor turns it to the eye's orientation, exactly, as it does without reprojection, and the
        // picture keeps all it has); mode 2, its pose now with the drawn frustum widened by kWiden (not the runtime's
        // whole one: the window's fitted frustum would be spread over it, blurred and mostly empty)
        XrPosef outPose = drawn[eye];
        XrFovf outFov = drawnFov;
        if (g.mode == 1) {
            outPose.position = now[eye].pose.position;
        } else {
            outPose = now[eye].pose;
            const float cx = 0.5f * (tl + tr), hx = 0.5f * (tr - tl) * kWiden;
            const float cy = 0.5f * (td + tu), hy = 0.5f * (tu - td) * kWiden;
            outFov = {std::atan(cx - hx), std::atan(cx + hx), std::atan(cy + hy), std::atan(cy - hy)};
        }
        const M4 clip = mul(mul(projection(std::tan(outFov.angleLeft), std::tan(outFov.angleRight),
                                           std::tan(outFov.angleDown), std::tan(outFov.angleUp)),
                                viewOf(outPose)),
                            fromPose(drawn[eye]));
        float cm[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) cm[j * 4 + i] = clip.m[i][j];
        glUniformMatrix4fv(g.uClip, 1, GL_FALSE, cm);
        glUniform4f(g.uTan, tl, tr, td, tu);
        glUniform4f(g.uDepthRect, float(eye * dEyeW), float(dEyeW), float(g.depthH), 1.0f / AURORA_XR_DEPTH_REF);
        glUniform1f(g.uEye, float(eye));
        glUniform1f(g.uHalfTexel, 0.5f / float(g.eyeW));
        // the refinement's line of sight: the eye now in the drawn eye's space (srcFromOut = view(drawn) *
        // pose(out)): its origin A, and the pixel's direction B = R * K * (fragX, fragY, 1), K taking the pixel
        // to the output frustum's tangents (x, y, -1) in this eye's viewport
        glUniform1i(g.uRefine, g.refine);
        const float ol = std::tan(outFov.angleLeft), orr = std::tan(outFov.angleRight);
        const float od = std::tan(outFov.angleDown), ou = std::tan(outFov.angleUp);
        const float kx = (orr - ol) / float(g.eyeW), ky = (ou - od) / float(g.eyeH);
        const float x0 = float(eye * g.eyeW);
        // K (row-major): (kx, 0, ol - x0 kx), (0, ky, od), (0, 0, -1)
        const float K[3][3] = {{kx, 0.0f, ol - x0 * kx}, {0.0f, ky, od}, {0.0f, 0.0f, -1.0f}};
        // R (the rotation of a rigid map from the eye now) * K, column-major for GLSL
        auto rayMatrix = [&](const M4& m, float* out) {
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) {
                    float v = 0.0f;
                    for (int k = 0; k < 3; k++) v += m.m[r][k] * K[k][c];
                    out[c * 3 + r] = v;
                }
        };
        if (g.refine > 0) {
            const M4 so = mul(viewOf(drawn[eye]), fromPose(outPose));
            float rm[9];
            rayMatrix(so, rm);
            glUniformMatrix3fv(g.uRayM, 1, GL_FALSE, rm);
            glUniform3f(g.uRayA, so.m[0][3], so.m[1][3], so.m[2][3]);
        }
        // the window (a frame drawn with its mask): its plane in the drawn eye's space as the frame had it, for the
        // texels outside its mask, and the eye now in the window's space where it stands now (`window`, else as
        // drawn) for the mask done again per pixel
        glUniform1i(g.uWindow, g.capWindow ? 1 : 0);
        if (g.capWindow) {
            const M4 ws = mul(viewOf(drawn[eye]), fromPose(g.capWindowPose));  // the drawn eye's from the window's
            const float n[3] = {ws.m[0][2], ws.m[1][2], ws.m[2][2]};  // its +z (toward the player)
            const float c = n[0] * ws.m[0][3] + n[1] * ws.m[1][3] + n[2] * ws.m[2][3];
            glUniform4f(g.uWinPlane, n[0], n[1], n[2], c);
            const bool now = window != nullptr;
            const M4 wo = mul(viewOf(now ? *window : g.capWindowPose), fromPose(outPose));  // the window's from now
            float wm[9];
            rayMatrix(wo, wm);
            glUniformMatrix3fv(g.uWinM, 1, GL_FALSE, wm);
            glUniform3f(g.uWinO, wo.m[0][3], wo.m[1][3], wo.m[2][3]);
            glUniform2f(g.uWinHalf, now ? windowHalfW : g.capWindowHalf[0], now ? windowHalfH : g.capWindowHalf[1]);
        }
        glViewport(GLint(eye * g.eyeW), 0, GLsizei(g.eyeW), GLsizei(g.eyeH));
        glDrawElements(GL_TRIANGLES, g.indexCount, GL_UNSIGNED_SHORT, nullptr);
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = outPose;
        views[eye].fov = outFov;
        views[eye].subImage.swapchain = g.chain;
        views[eye].subImage.imageRect = {{int32_t(eye * g.eyeW), 0}, {int32_t(g.eyeW), int32_t(g.eyeH)}};
    }
    // the depth was only for the folds: not stored
    const GLenum discard = GL_DEPTH_ATTACHMENT;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &discard);
    if (query >= 0) {
        glEndQuery(GL_TIME_ELAPSED_EXT);
        g.queryBusy[query] = true;
    }
    glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g.chain, &rel);
    g.cpuNs += nowNs() - cpuStart;
    g.drawn++;
    return true;
}

}  // namespace wqr
