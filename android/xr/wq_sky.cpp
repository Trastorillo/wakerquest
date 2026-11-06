// Techo abierto (roof_open, 1.2.3): see wq_sky.h.
//
// A projection layer right over the room (XR_FB_passthrough), its alpha 0 where the room stays. Each pixel is a ray
// from the eye against a round room of no real geometry (the headset's room is not scanned): one that clears the
// walls' top, or goes up through the opened ceiling, sees the sky, or the sea far below the floor when it points
// down, so more sea shows the closer one leans over the wall. The colours are the game's sky while outdoors
// (wq_xr_set_weather), and the last ones elsewhere: the surroundings stay wherever Link goes.
#include "wq_sky.h"
#include "wq_xr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <SDL3/SDL.h>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr int kEyeSize = 1152;       // pixels a side, per eye
constexpr int kMaxWalls = 24;
constexpr float kCeiling = 2.6f;     // m above the floor, where the ceiling opens
constexpr float kOpenSeconds = 5.0f;  // the ceiling, then the walls

const char* const kVs = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* const kFs = R"(#version 300 es
precision highp float;
precision highp int;  // by default 16 bits in a fragment shader on the headset's GPU: the hash below needs all 32
out vec4 o_color;
// Each pixel's ray from its own position, not from a value interpolated over the triangle: the straight cuts and the
// blocks seen on the headset look like coordinates that lost precision on the way
uniform vec4 u_tan;   // left, right, down, up
uniform mat3 u_rot;
uniform vec4 u_view;  // the eye's picture in the image: x, y, width, height
uniform vec3 u_eye;    // from the room's middle at floor level
uniform vec4 u_room;   // radius, the walls' top, the ceiling, the opening's half width
uniform vec4 u_walls[24];  // the headset's room: each wall's ends (x, z, x, z)
uniform int u_wallCount;   // 0: a circle of u_room.x
uniform vec3 u_zenith, u_horizon, u_cloud, u_sea;  // linear
uniform vec4 u_sun;    // direction, strength
uniform vec4 u_bolt;   // azimuth, seed, strength
uniform float u_time, u_storm, u_flash, u_night, u_encode;
uniform vec3 u_seaDif;

const float kSeaDepth = 4.0;  // m, the sea under the floor

// In integers: the same corner gives the very same value from every cell. (A float hash here left seams on the
// headset's GPU, which rounds the same sum differently from one place in the code to another.)
float hash(vec2 p) {
    // the offset and the mixing between x and y: without them a row of cells at 0 came out alike, a line of stars
    uvec2 q = uvec2(ivec2(p)) + uvec2(40503u, 31153u);
    uint h = q.x * 1597334677u;
    h ^= h >> 16;
    h += q.y;
    h *= 3812015801u;
    h ^= h >> 15;
    h *= 1597334677u;
    h ^= h >> 13;
    return float(h >> 8) / 16777215.0;
}
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
// Each octave fades as it nears a pixel's size, toward its average
float fbm(vec2 p) {
    float a = 0.5, v = 0.0;
    float px = length(fwidth(p));
    for (int i = 0; i < 4; i++) {
        v += a * mix(noise(p), 0.5, smoothstep(0.2, 0.6, px));
        p = p * 2.03 + vec2(17.1, 9.2);
        px *= 2.03;
        a *= 0.5;
    }
    return v;
}

// 1 where the ray leaves the room: over the walls, or through the opening in the ceiling
float roomOpen(vec3 d) {
    float t = -1.0;
    if (u_wallCount > 0) {
        // the nearest wall ahead (none: a gap in the walls, open)
        for (int i = 0; i < u_wallCount; i++) {
            vec2 s = u_walls[i].zw - u_walls[i].xy;
            vec2 o = u_eye.xz - u_walls[i].xy;
            float den = d.x * s.y - d.z * s.x;
            if (abs(den) < 1e-6) continue;
            float hit = -(o.x * s.y - o.y * s.x) / den;
            float along = (o.x * d.z - o.y * d.x) / (s.x * d.z - s.y * d.x);
            if (hit > 0.0 && along >= -0.01 && along <= 1.01 && (t < 0.0 || hit < t)) t = hit;
        }
        if (t < 0.0) return 1.0;
    } else {
        float a = max(dot(d.xz, d.xz), 1e-5);
        float b = dot(u_eye.xz, d.xz);
        float c = dot(u_eye.xz, u_eye.xz) - u_room.x * u_room.x;
        float disc = b * b - a * c;
        if (disc < 0.0) return 1.0;
        t = (-b + sqrt(disc)) / a;
        if (t < 0.0) return 1.0;
    }
    float h = u_eye.y + d.y * t;
    if (h > u_room.z && d.y > 1e-4) {
        vec3 p = u_eye + d * ((u_room.z - u_eye.y) / d.y);
        return 1.0 - smoothstep(u_room.w - 0.02, u_room.w + 0.02, abs(p.x));
    }
    float e = 0.015 + 0.002 * t;
    return smoothstep(u_room.y - e, u_room.y + e, h);
}

vec3 skyColor(vec3 d) {
    float y = max(d.y, 0.0);
    vec3 c = mix(u_horizon, u_zenith, 1.0 - exp(-y * 4.5));
    c = mix(c, vec3(0.035, 0.04, 0.05), u_storm * 0.85);
    float s = max(dot(d, u_sun.xyz), 0.0);
    c += u_sun.w * (1.0 - 0.9 * u_storm) *
         (vec3(1.0, 0.85, 0.6) * pow(s, 64.0) * 0.35 + vec3(1.0, 0.95, 0.8) * smoothstep(0.9985, 0.9992, s) * 3.0);
    return c;
}

// A crescent moon at night, across the sky from where the sun went down: one disc less another, with a soft glow
vec3 withMoon(vec3 c, vec3 d) {
    vec3 m = normalize(vec3(-u_sun.x, max(-u_sun.y, 0.3), -0.35));
    float along = dot(d, m);
    // far from it q runs away, and with it the pixel's size below: a bright line across the sky
    if (along <= 0.7) return c;
    vec3 side = normalize(cross(m, vec3(0.0, 1.0, 0.0)));
    vec3 up = cross(side, m);
    vec2 q = vec2(dot(d, side), dot(d, up)) / along;
    const float r = 0.06;
    float e = max(fwidth(q.y) * 1.5, 0.001);
    float disc = 1.0 - smoothstep(r - e, r + e, length(q));
    float bite = 1.0 - smoothstep(r * 0.86 - e, r * 0.86 + e, length(q - vec2(r * 0.42, r * 0.16)));
    vec3 glow = vec3(0.5, 0.55, 0.7) * 0.25 * exp(-length(q) * 9.0);
    return c + u_night * (1.0 - u_storm) * (glow + vec3(1.0, 0.93, 0.7) * 1.3 * disc * (1.0 - bite));
}

vec3 withClouds(vec3 c, vec3 d) {
    if (d.y < 0.015) return c;
    vec2 p = d.xz / (d.y + 0.12) * 1.4 + vec2(0.012, 0.005) * u_time * (1.0 + 2.0 * u_storm);
    float n = fbm(p);
    float cover = mix(0.56, 0.30, u_storm);
    float soft = max(mix(0.1, 0.2, u_storm), fwidth(n) * 2.0);
    float m = smoothstep(cover, cover + soft, n) * smoothstep(0.015, 0.14, d.y);
    vec3 cc = mix(u_cloud, vec3(0.018, 0.02, 0.026), u_storm * 0.92);
    cc *= mix(0.72, 1.05, smoothstep(cover, cover + 0.35, n));
    c = mix(c, cc, m * 0.92);
    return c + u_flash * vec3(0.55, 0.62, 0.85) * (0.12 + 0.45 * m);
}

// One layer of crests: at most one in each cell of a grid, a thin curved stroke that fades in and out by itself
float crestLayer(vec2 w, float cell, float seed) {
    vec2 g = w / cell + seed;
    vec2 id = floor(g);
    vec2 f = fract(g) - 0.5;
    float h1 = hash(id + seed), h2 = hash(id + vec2(7.0, 3.0)), h3 = hash(id + vec2(13.0, 29.0));
    if (h3 < mix(0.45, 0.12, u_storm)) return 0.0;
    float life = sin(u_time * (0.2 + 0.2 * h1) + h2 * 6.2831853);
    if (life <= 0.0) return 0.0;
    vec2 q = f - (vec2(h1, h2) - 0.5) * 0.2;
    float x = q.x / (0.26 + 0.12 * h3);
    if (abs(x) >= 1.0) return 0.0;
    float y = q.y - (0.05 + 0.05 * h2) * (x * x - 0.5);
    float thick = mix(0.012, 0.03, u_storm) * (1.0 - x * x);
    // a pixel's size in the cell, from the whole sea's coordinates: no jump at the cells' edges
    float aa = length(fwidth(w)) / cell;
    return (1.0 - smoothstep(thick, thick + aa, abs(y))) * smoothstep(0.0, 0.5, life);
}

// The sea: its colour from the game, soft swells of a slightly paler tone that move, and loose white crests.
vec3 seaColor(vec3 d) {
    float t = (u_eye.y + kSeaDepth) / max(-d.y, 1e-4);
    vec2 w = u_eye.xz + d.xz * t;
    vec3 deep = u_seaDif * mix(1.0, 0.55, u_storm);
    float far = exp(-t * 0.012);
    float rough = 1.0 + 0.8 * u_storm;
    float s = sin(dot(w, vec2(0.8, 0.6)) * 0.35 * rough + u_time * 0.6) +
              0.6 * sin(dot(w, vec2(-0.5, 0.85)) * 0.6 * rough + u_time * 0.9) +
              0.3 * sin(dot(w, vec2(0.2, -0.97)) * 1.1 * rough + u_time * 1.2);
    vec3 c = mix(deep, deep * 1.25 + vec3(0.006, 0.03, 0.04), smoothstep(-0.3, 1.1, s) * far);
    float light = dot(mix(u_horizon, u_zenith, 0.5), vec3(0.3, 0.6, 0.1));
    float k = max(crestLayer(w, 16.0, 0.0), crestLayer(w, 27.0, 0.37)) * exp(-t * 0.008);
    c = mix(c, vec3(0.92, 0.96, 1.0) * clamp(light * 1.6 + 0.12, 0.06, 1.0), k * 0.9);
    c = mix(skyColor(normalize(vec3(d.x, 0.0, d.z))), c, exp(-t * 0.0035));
    return c + u_flash * vec3(0.55, 0.62, 0.85) * 0.12;
}

void main() {
    vec2 pix = (gl_FragCoord.xy - u_view.xy) / u_view.zw;
    vec3 d = normalize(u_rot * vec3(mix(u_tan.x, u_tan.y, pix.x), mix(u_tan.z, u_tan.w, pix.y), -1.0));
    float m = roomOpen(d);
    if (m <= 0.001) {
        o_color = vec4(0.0);
        return;
    }
    vec3 c;
    if (d.y >= 0.0) {
        c = skyColor(d);
        if (u_night > 0.01) {
            vec2 q = d.xz / (d.y + 0.3) * 90.0;
            // two hashes together: one alone lines stars up along a diagonal of cells
            vec2 cellId = floor(q);
            float h = step(0.9, hash(cellId)) * step(0.95, hash(cellId.yx * vec2(3.0, 7.0) + vec2(11.0, 5.0)));
            c += u_night * (1.0 - u_storm) * vec3(0.8, 0.85, 1.0) * h *
                 (1.0 - smoothstep(0.1, 0.45, length(fract(q) - 0.5)));
        }
        if (u_night > 0.01) c = withMoon(c, d);
        c = withClouds(c, d);
    } else {
        c = seaColor(d);
    }
    if (u_bolt.z > 0.01 && d.y > 0.0 && d.y < 0.5) {
        float az = atan(d.x, d.z);
        float e = asin(d.y);
        float x = (noise(vec2(e * 9.0, u_bolt.y)) - 0.5) * 0.16 + (noise(vec2(e * 38.0, u_bolt.y)) - 0.5) * 0.04;
        float da = abs(mod(az - u_bolt.x - x + 3.14159265, 6.2831853) - 3.14159265);
        c += u_bolt.z * (vec3(0.95, 0.97, 1.0) * (1.0 - smoothstep(0.001, 0.004, da)) * 2.0 +
                         vec3(0.4, 0.45, 0.7) * (1.0 - smoothstep(0.0, 0.05, da)) * 0.35);
    }
    if (u_encode > 0.5) c = pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));
    o_color = vec4(c * m, m);
}
)";

struct Weather {
    float zenith[3] = {50, 120, 210}, horizon[3] = {170, 210, 235}, cloud[3] = {245, 245, 240}, sea[3] = {20, 90, 140};
    float storm = 0.0f, flash = 0.0f, time = 180.0f;
    float seaDif[3] = {20, 90, 140};
};
std::mutex s_weatherLock;
Weather s_weatherIn;

struct {
    XrSwapchain chain = XR_NULL_HANDLE;
    bool failed = false;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    GLuint prog = 0, fbo = 0;
    float amount = 0.0f;  // 0 shut, 1 open
    float clock = 0.0f;
    Weather now;  // eased, sRGB 0-255 colours
    bool wasFlash = false;
    float boltAzimuth = 0.0f, boltSeed = 0.0f;
} g;

GLuint compileShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint okc = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &okc);
    if (!okc) {
        char log[1024] = "";
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        fprintf(stderr, "[xr] open roof shader: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

bool init(XrSession session, bool srgb) {
    if (g.failed) return false;
    if (g.chain != XR_NULL_HANDLE) return true;
    g.failed = true;  // until it all works: no second try every frame
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVs), fs = compileShader(GL_FRAGMENT_SHADER, kFs);
    if (vs == 0 || fs == 0) return false;
    g.prog = glCreateProgram();
    glAttachShader(g.prog, vs);
    glAttachShader(g.prog, fs);
    glLinkProgram(g.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(g.prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = "";
        glGetProgramInfoLog(g.prog, sizeof log, nullptr, log);
        fprintf(stderr, "[xr] open roof program: %s\n", log);
        return false;
    }
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sc.format = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    sc.sampleCount = 1;
    sc.width = 2 * kEyeSize;
    sc.height = kEyeSize;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &sc, &g.chain))) {
        fprintf(stderr, "[xr] open roof: xrCreateSwapchain failed\n");
        g.chain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g.chain, 0, &n, nullptr);
    g.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(g.chain, n, &n, (XrSwapchainImageBaseHeader*)g.images.data());
    glGenFramebuffers(1, &g.fbo);
    g.failed = false;
    fprintf(stderr, "[xr] open roof: ready (%d a side per eye)\n", kEyeSize);
    return true;
}

// ---- The room the headset knows (Space Setup: XR_FB_scene): its walls, floor and ceiling. Needs the USE_SCENE
// permission and a room set up; without them the circle of roof_radius, and the log says why.
struct SceneSpace {
    XrSpace space;
    int kind;  // 0 wall, 1 floor, 2 ceiling
    XrRect2Df box;
};
struct {
    int state = 0;  // 0 not asked, 1 asking the permission, 2 allowed, 3 querying, 4 done, 5 none
    XrInstance instance = XR_NULL_HANDLE;
    XrAsyncRequestIdFB request = 0;
    std::vector<SceneSpace> spaces;
    int tick = 0;
    PFN_xrQuerySpacesFB query = nullptr;
    PFN_xrRetrieveSpaceQueryResultsFB retrieve = nullptr;
    PFN_xrGetSpaceSemanticLabelsFB labels = nullptr;
    PFN_xrGetSpaceBoundingBox2DFB box = nullptr;
    PFN_xrSetSpaceComponentStatusFB setStatus = nullptr;
    // what is shown, in the space, from the last location of the spaces
    float walls[kMaxWalls][4];
    int wallCount = 0;
    bool haveFloor = false, haveCeiling = false;
    float floorY = 0.0f, ceilingY = 0.0f;
} g_scene;

void onScenePermission(void*, const char*, bool granted) {
    fprintf(stderr, "[xr] room: USE_SCENE permission %s\n", granted ? "granted" : "refused");
    g_scene.state = granted ? 2 : 5;
}

void sceneQuery(XrSession session) {
    auto proc = [&](const char* name, PFN_xrVoidFunction* to) { xrGetInstanceProcAddr(g_scene.instance, name, to); };
    proc("xrQuerySpacesFB", (PFN_xrVoidFunction*)&g_scene.query);
    proc("xrRetrieveSpaceQueryResultsFB", (PFN_xrVoidFunction*)&g_scene.retrieve);
    proc("xrGetSpaceSemanticLabelsFB", (PFN_xrVoidFunction*)&g_scene.labels);
    proc("xrGetSpaceBoundingBox2DFB", (PFN_xrVoidFunction*)&g_scene.box);
    proc("xrSetSpaceComponentStatusFB", (PFN_xrVoidFunction*)&g_scene.setStatus);
    if (!g_scene.query || !g_scene.retrieve || !g_scene.labels || !g_scene.box || !g_scene.setStatus) {
        fprintf(stderr, "[xr] room: scene functions missing: query %d retrieve %d labels %d box %d status %d\n",
                g_scene.query != nullptr, g_scene.retrieve != nullptr, g_scene.labels != nullptr, g_scene.box != nullptr,
                g_scene.setStatus != nullptr);
        g_scene.state = 5;
        return;
    }
    XrSpaceStorageLocationFilterInfoFB where{XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB};
    where.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
    XrSpaceComponentFilterInfoFB filter{XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB};
    filter.next = &where;
    filter.componentType = XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB;
    XrSpaceQueryInfoFB info{XR_TYPE_SPACE_QUERY_INFO_FB};
    info.queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB;
    info.maxResultCount = 256;
    info.timeout = XR_INFINITE_DURATION;
    info.filter = (const XrSpaceFilterInfoBaseHeaderFB*)&filter;
    if (XR_FAILED(g_scene.query(session, (const XrSpaceQueryInfoBaseHeaderFB*)&info, &g_scene.request))) {
        fprintf(stderr, "[xr] room: the query failed\n");
        g_scene.state = 5;
        return;
    }
    g_scene.state = 3;
}

void sceneResults(XrSession session) {
    XrSpaceQueryResultsFB res{XR_TYPE_SPACE_QUERY_RESULTS_FB};
    if (XR_FAILED(g_scene.retrieve(session, g_scene.request, &res))) return;
    std::vector<XrSpaceQueryResultFB> found(res.resultCountOutput);
    res.resultCapacityInput = uint32_t(found.size());
    res.results = found.data();
    if (XR_FAILED(g_scene.retrieve(session, g_scene.request, &res))) return;
    for (const XrSpaceQueryResultFB& r : found) {
        char buffer[256] = "";
        XrSemanticLabelsFB labels{XR_TYPE_SEMANTIC_LABELS_FB};
        labels.bufferCapacityInput = sizeof buffer - 1;
        labels.buffer = buffer;
        if (XR_FAILED(g_scene.labels(session, r.space, &labels))) continue;
        buffer[std::min<uint32_t>(labels.bufferCountOutput, sizeof buffer - 1)] = 0;
        const int kind = strstr(buffer, "WALL_FACE") ? 0 : strstr(buffer, "FLOOR") ? 1 : strstr(buffer, "CEILING") ? 2 : -1;
        if (kind < 0) continue;
        SceneSpace sp{r.space, kind, {}};
        if (XR_FAILED(g_scene.box(session, r.space, &sp.box))) continue;
        XrSpaceComponentStatusSetInfoFB on{XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB};
        on.componentType = XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB;
        on.enabled = XR_TRUE;
        XrAsyncRequestIdFB ignored = 0;
        g_scene.setStatus(r.space, &on, &ignored);  // already on is fine: it is located below or not
        g_scene.spaces.push_back(sp);
    }
}

XrVector3f poseAt(const XrPosef& pose, float x, float y) {
    const XrQuaternionf& q = pose.orientation;
    const float tx = 2 * (q.y * 0.0f - q.z * y), ty = 2 * (q.z * x - q.x * 0.0f), tz = 2 * (q.x * y - q.y * x);
    return {pose.position.x + x + q.w * tx + (q.y * tz - q.z * ty),
            pose.position.y + y + q.w * ty + (q.z * tx - q.x * tz),
            pose.position.z + q.w * tz + (q.x * ty - q.y * tx)};
}

// Where the walls are now in the space (it moves when the headset is recentred)
void sceneLocate(XrSpace space, XrTime time) {
    int walls = 0;
    bool floorOk = false, ceilOk = false;
    float floorY = 0.0f, ceilY = 0.0f;
    for (const SceneSpace& sp : g_scene.spaces) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(sp.space, space, time, &loc)) ||
            !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
            !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
            continue;
        if (sp.kind == 1) floorOk = true, floorY = loc.pose.position.y;
        if (sp.kind == 2) ceilOk = true, ceilY = loc.pose.position.y;
        if (sp.kind == 0 && walls < kMaxWalls) {
            const XrVector3f a = poseAt(loc.pose, sp.box.offset.x, sp.box.offset.y);
            const XrVector3f b = poseAt(loc.pose, sp.box.offset.x + sp.box.extent.width, sp.box.offset.y);
            g_scene.walls[walls][0] = a.x, g_scene.walls[walls][1] = a.z;
            g_scene.walls[walls][2] = b.x, g_scene.walls[walls][3] = b.z;
            walls++;
        }
    }
    g_scene.wallCount = walls;
    g_scene.haveFloor = floorOk, g_scene.floorY = floorY;
    g_scene.haveCeiling = ceilOk, g_scene.ceilingY = ceilY;
}

float linear(float srgb255) { return std::pow(std::clamp(srgb255 / 255.0f, 0.0f, 1.0f), 2.2f); }

void setColor(const char* name, const float* srgb255) {
    glUniform3f(glGetUniformLocation(g.prog, name), linear(srgb255[0]), linear(srgb255[1]), linear(srgb255[2]));
}

void ease(float* to, const float* from, int n, float k) {
    for (int i = 0; i < n; i++) to[i] += (from[i] - to[i]) * k;
}

float smooth01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

}  // namespace

extern "C" void wq_xr_set_sea_color(const float* color) {
    std::lock_guard lock{s_weatherLock};
    std::copy(color, color + 3, s_weatherIn.seaDif);
}

extern "C" void wq_xr_set_weather(const float* colors, float storm, float flash, float daytime) {
    std::lock_guard lock{s_weatherLock};
    if (colors == nullptr && storm < 0.0f) {  // not outdoors: the last colours and weather, no lightning
        s_weatherIn.flash = 0.0f;
        return;
    }
    if (colors != nullptr) {
        std::copy(colors, colors + 3, s_weatherIn.zenith);
        std::copy(colors + 3, colors + 6, s_weatherIn.horizon);
        std::copy(colors + 6, colors + 9, s_weatherIn.cloud);
        std::copy(colors + 9, colors + 12, s_weatherIn.sea);
    }
    s_weatherIn.storm = storm;
    s_weatherIn.flash = flash;
    s_weatherIn.time = daytime;
}

namespace wqs {

void sceneEvent(XrSession session, const XrEventDataBuffer& ev) {
    if (ev.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB) {
        const auto& e = *reinterpret_cast<const XrEventDataSpaceQueryResultsAvailableFB*>(&ev);
        if (g_scene.state == 3 && e.requestId == g_scene.request) sceneResults(session);
    } else if (ev.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB) {
        const auto& e = *reinterpret_cast<const XrEventDataSpaceQueryCompleteFB*>(&ev);
        if (g_scene.state == 3 && e.requestId == g_scene.request) {
            g_scene.state = 4;
            fprintf(stderr, "[xr] room: %d walls, floor and ceiling from the headset's Space Setup%s\n",
                    int(g_scene.spaces.size()), g_scene.spaces.empty() ? " (none: no room set up)" : "");
        }
    }
}

void sceneTick(XrInstance instance, XrSession session, XrSpace space, XrTime time, bool want) {
    if (!want) return;
    g_scene.instance = instance;
    if (g_scene.state == 0) {
        g_scene.state = 1;
        fprintf(stderr, "[xr] room: asking for the USE_SCENE permission\n");
        if (!SDL_RequestAndroidPermission("com.oculus.permission.USE_SCENE", onScenePermission, nullptr)) g_scene.state = 5;
    } else if (g_scene.state == 2) {
        sceneQuery(session);
    }
    if (g_scene.tick-- <= 0) {
        g_scene.tick = 45;
        sceneLocate(space, time);
    }
}

bool layer(XrSession session, XrSpace space, XrTime time, bool srgb, bool open, const Room& room, int hz,
           XrCompositionLayerProjection* out, XrCompositionLayerProjectionView* views) {
    const float dt = hz > 0 ? 1.0f / float(hz) : 1.0f / 72.0f;
    g.amount = std::clamp(g.amount + (open ? dt : -dt) / kOpenSeconds, 0.0f, 1.0f);
    if (g.amount <= 0.0f || !init(session, srgb)) return false;

    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = time;
    li.space = space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView eyes[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(session, &li, &vs, 2, &n, eyes)) || n != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
        return false;

    Weather in;
    {
        std::lock_guard lock{s_weatherLock};
        in = s_weatherIn;
    }
    g.clock += dt;
    if (g.clock > 3600.0f) g.clock -= 3600.0f;
    const float k = 1.0f - std::exp(-dt / 1.5f);
    ease(g.now.zenith, in.zenith, 3, k);
    ease(g.now.horizon, in.horizon, 3, k);
    ease(g.now.cloud, in.cloud, 3, k);
    ease(g.now.sea, in.sea, 3, k);
    ease(g.now.seaDif, in.seaDif, 3, k);
    g.now.storm += (in.storm - g.now.storm) * (1.0f - std::exp(-dt / 4.0f));
    const bool flashing = in.flash > 0.3f;
    if (flashing && !g.wasFlash) {  // a new bolt somewhere else
        g.boltAzimuth = float(rand() % 6283) * 0.001f - 3.14159f;
        g.boltSeed = float(rand() % 1000) * 0.1f;
    }
    g.wasFlash = flashing;
    // the sun by the game's clock (daytime in degrees: 90 at dawn, 180 at noon), rising on one side of the room
    const float a = (in.time - 90.0f) * 0.0174532925f;
    float sun[3] = {std::cos(a), std::sin(a), -0.35f};
    const float len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    for (float& v : sun) v /= len;
    const float sunUp = std::clamp(sun[1] * 6.0f + 0.2f, 0.0f, 1.0f);
    const float night = std::clamp(-sun[1] * 4.0f, 0.0f, 1.0f);

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
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.images[idx].image, 0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(g.prog);
    const float floorY = g_scene.haveFloor ? g_scene.floorY : room.floorY;
    const int walls = g_scene.wallCount;
    // the walls (and the eyes below) from the room's middle at floor level
    float wallsHere[kMaxWalls][4];
    for (int i = 0; i < walls; i++) {
        wallsHere[i][0] = g_scene.walls[i][0] - room.centre[0], wallsHere[i][1] = g_scene.walls[i][1] - room.centre[1];
        wallsHere[i][2] = g_scene.walls[i][2] - room.centre[0], wallsHere[i][3] = g_scene.walls[i][3] - room.centre[1];
    }
    glUniform1i(glGetUniformLocation(g.prog, "u_wallCount"), walls);
    if (walls > 0) glUniform4fv(glGetUniformLocation(g.prog, "u_walls"), walls, &wallsHere[0][0]);
    const float ceiling = g_scene.haveCeiling ? std::max(g_scene.ceilingY - floorY, room.wall + 0.2f) : kCeiling;
    // the opening's half width: the ceiling's whole span once open, wherever the walls stand
    float span = room.radius + 0.1f;
    for (int i = 0; i < walls; i++)
        span = std::max({span, std::fabs(wallsHere[i][0]) + 0.1f, std::fabs(wallsHere[i][2]) + 0.1f});
    const float opening = smooth01(g.amount / 0.45f) * span;
    const float wallTop = ceiling + (room.wall - ceiling) * smooth01((g.amount - 0.4f) / 0.6f);
    glUniform4f(glGetUniformLocation(g.prog, "u_room"), room.radius, wallTop, ceiling, opening);
    setColor("u_zenith", g.now.zenith);
    setColor("u_horizon", g.now.horizon);
    setColor("u_cloud", g.now.cloud);
    setColor("u_sea", g.now.sea);
    setColor("u_seaDif", g.now.seaDif);
    glUniform4f(glGetUniformLocation(g.prog, "u_sun"), sun[0], sun[1], sun[2], sunUp);
    glUniform4f(glGetUniformLocation(g.prog, "u_bolt"), g.boltAzimuth, g.boltSeed, in.flash, 0.0f);
    glUniform1f(glGetUniformLocation(g.prog, "u_time"), g.clock);
    glUniform1f(glGetUniformLocation(g.prog, "u_storm"), g.now.storm);
    glUniform1f(glGetUniformLocation(g.prog, "u_flash"), in.flash);
    glUniform1f(glGetUniformLocation(g.prog, "u_night"), night);
    glUniform1f(glGetUniformLocation(g.prog, "u_encode"), srgb ? 0.0f : 1.0f);
    for (int eye = 0; eye < 2; eye++) {
        const XrQuaternionf& q = eyes[eye].pose.orientation;
        // the view's rotation, column by column
        const float rot[9] = {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.w * q.z), 2 * (q.x * q.z - q.w * q.y),
                              2 * (q.x * q.y - q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.w * q.x),
                              2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)};
        glUniformMatrix3fv(glGetUniformLocation(g.prog, "u_rot"), 1, GL_FALSE, rot);
        const XrFovf& f = eyes[eye].fov;
        glUniform4f(glGetUniformLocation(g.prog, "u_tan"), std::tan(f.angleLeft), std::tan(f.angleRight),
                    std::tan(f.angleDown), std::tan(f.angleUp));
        const XrVector3f& p = eyes[eye].pose.position;
        glUniform3f(glGetUniformLocation(g.prog, "u_eye"), p.x - room.centre[0], p.y - floorY, p.z - room.centre[1]);
        glUniform4f(glGetUniformLocation(g.prog, "u_view"), float(eye * kEyeSize), 0.0f, float(kEyeSize), float(kEyeSize));
        glViewport(eye * kEyeSize, 0, kEyeSize, kEyeSize);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = eyes[eye].pose;
        views[eye].fov = f;
        views[eye].subImage.swapchain = g.chain;
        views[eye].subImage.imageRect = {{eye * kEyeSize, 0}, {kEyeSize, kEyeSize}};
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g.chain, &rel);

    *out = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = space;
    out->viewCount = 2;
    out->views = views;
    return true;
}

}  // namespace wqs
