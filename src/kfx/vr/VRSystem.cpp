/******************************************************************************/
// Dungeon Keeper - VR mode
/******************************************************************************/
/** @file VRSystem.cpp
 *     OpenXR session and the VR tabletop pass. See VRSystem.h.
 * @par Controls:
 *     Grip (either hand) and move: drag the dungeon around.
 *     Both grips: pull apart / push together to scale, twist to rotate.
 *     A (right controller): put the dungeon back in front of you.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/vr/VR.h"
#include "kfx/vr/VRSystem.h"

int vr_requested = 0;

#ifdef KFX_VR

// Windows and OpenXR first: some game headers trim what windows.h declares.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// No prototypes: every OpenXR function is a pointer fetched from the runtime
// (see load_runtime()), so the names below are variables in this file.
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "kfx/vr/VRMath.h"
#include "kfx/vr/VRDungeonMesh.h"
#include "kfx/vr/VRThings.h"
#include "kfx/vr/VRInput.h"
#include "kfx/renderer/opengl/GLFunctions.h"
#include "kfx/renderer/opengl/GLWorldViewRenderer.h"
#include "kfx/renderer/opengl/GLShaders.h"   // WORLD_FRAGMENT_SHADER
#include "kfx/renderer/RendererSettings.h"   // g_renderer_settings
#include "globals.h"                         // ERRORLOG / SYNCLOG
#include "bflib_basics.h"
#include "bflib_math.h"                      // LbArcTanAngle()
#include "bflib_sndlib.h"                    // FollowDefaultAudioDevice()

// OpenXR structs are initialised the idiomatic way, {XR_TYPE_...}, leaving
// the remaining fields zero.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "post_inc.h"

namespace VR {

namespace {

/******************************************************************************/
// OpenXR runtime loading
//
// Instead of the Khronos loader, the active runtime is loaded directly: its
// manifest path comes from XR_RUNTIME_JSON or the ActiveRuntime registry value
// (a 32-bit process reads the WOW6432Node copy automatically), and its
// xrNegotiateLoaderRuntimeInterface export provides xrGetInstanceProcAddr.

#define VR_XR_INSTANCE_FUNCTIONS(X) \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrResultToString) X(xrGetSystem) \
    X(xrCreateSession) X(xrCreateReferenceSpace) X(xrEnumerateViewConfigurationViews) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrEnumerateSwapchainImages) \
    X(xrCreateActionSet) X(xrStringToPath) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) X(xrCreateActionSpace) X(xrPollEvent) X(xrBeginSession) \
    X(xrEndSession) X(xrLocateSpace) X(xrSyncActions) X(xrGetActionStateFloat) \
    X(xrGetActionStateBoolean) X(xrGetActionStateVector2f) X(xrLocateViews) X(xrAcquireSwapchainImage) \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)

#define VR_XR_DECLARE(name) PFN_##name name = nullptr;
PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
PFN_xrCreateInstance xrCreateInstance = nullptr;
VR_XR_INSTANCE_FUNCTIONS(VR_XR_DECLARE)
#undef VR_XR_DECLARE

HMODULE s_runtime_dll = nullptr;

std::string active_runtime_manifest()
{
    const char* env = std::getenv("XR_RUNTIME_JSON");
    if (env != nullptr && env[0] != '\0')
        return env;
    char path[MAX_PATH] = "";
    DWORD size = sizeof(path);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime",
                     RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
        return std::string();
    return path;
}

/** Reads "library_path" from a runtime manifest, resolved against the manifest's folder. */
std::string runtime_library_path(const std::string& manifest)
{
    FILE* f = std::fopen(manifest.c_str(), "rb");
    if (f == nullptr)
        return std::string();
    std::string json;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        json.append(buf, n);
    std::fclose(f);

    size_t pos = json.find("\"library_path\"");
    if (pos == std::string::npos) return std::string();
    pos = json.find(':', pos);
    if (pos == std::string::npos) return std::string();
    pos = json.find('"', pos);
    if (pos == std::string::npos) return std::string();
    std::string lib;
    for (size_t i = pos + 1; i < json.size() && json[i] != '"'; i++)
    {
        if (json[i] == '\\' && i + 1 < json.size())
            i++;  // JSON escape: \\ -> \, \/ -> /
        lib += json[i];
    }
    const bool absolute = (lib.size() > 1 && lib[1] == ':') || (!lib.empty() && (lib[0] == '\\' || lib[0] == '/'));
    if (!absolute)
    {
        const size_t slash = manifest.find_last_of("\\/");
        if (slash != std::string::npos)
            lib = manifest.substr(0, slash + 1) + lib;
    }
    return lib;
}

bool load_runtime()
{
    if (xrCreateInstance != nullptr)
        return true;  // already loaded by an earlier attempt
    const std::string manifest = active_runtime_manifest();
    if (manifest.empty())
    {
        ERRORLOG("VR: no OpenXR runtime is set as active (install SteamVR, Virtual Desktop or the Meta app)");
        return false;
    }
    const std::string library = runtime_library_path(manifest);
    SYNCLOG("VR: OpenXR runtime manifest \"%s\", library \"%s\"", manifest.c_str(), library.c_str());
    s_runtime_dll = LoadLibraryExA(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (s_runtime_dll == nullptr)
    {
        ERRORLOG("VR: could not load the OpenXR runtime (error %lu) -- does it have a 32-bit version?",
                 (unsigned long)GetLastError());
        return false;
    }
    PFN_xrNegotiateLoaderRuntimeInterface negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)
        (void*)GetProcAddress(s_runtime_dll, "xrNegotiateLoaderRuntimeInterface");
    if (negotiate == nullptr)
    {
        ERRORLOG("VR: runtime has no xrNegotiateLoaderRuntimeInterface");
        return false;
    }
    XrNegotiateLoaderInfo loader_info = {};
    loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader_info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loader_info.structSize = sizeof(loader_info);
    loader_info.minInterfaceVersion = 1;
    loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader_info.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest request = {};
    request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);
    if (XR_FAILED(negotiate(&loader_info, &request)) || request.getInstanceProcAddr == nullptr)
    {
        ERRORLOG("VR: runtime negotiation failed");
        return false;
    }
    xrGetInstanceProcAddr = request.getInstanceProcAddr;
    if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance))
        || xrCreateInstance == nullptr)
    {
        ERRORLOG("VR: runtime does not provide xrCreateInstance");
        return false;
    }
    return true;
}

bool load_instance_functions(XrInstance instance)
{
#define VR_XR_LOAD(name) \
    if (XR_FAILED(xrGetInstanceProcAddr(instance, #name, (PFN_xrVoidFunction*)&name)) || name == nullptr) \
    { \
        ERRORLOG("VR: runtime does not provide " #name); \
        return false; \
    }
    VR_XR_INSTANCE_FUNCTIONS(VR_XR_LOAD)
#undef VR_XR_LOAD
    return true;
}

/******************************************************************************/
// Shaders

/* World geometry in world space. Feeds WORLD_FRAGMENT_SHADER the same
 * varyings as WORLD_VERTEX_SHADER, so tiles look exactly like the desktop
 * view. Lighting comes from the game's live subtile lightmap. */
const char* const k_world_vs = R"glsl(
#version 330 core
layout(location = 0) in vec3  a_pos;
layout(location = 1) in vec2  a_uv;
layout(location = 2) in float a_shade;
layout(location = 3) in vec2  a_stl;
layout(location = 4) in float a_layer;
layout(location = 5) in float a_tile;
uniform mat4       u_mvp;
uniform usampler2D u_lightmap;        // game.lish.subtile_lightness, 511 texels per row
uniform int        u_lightmap_stride; // subtiles per lightness row
uniform float      u_min_shade;       // keeps unlit rock visible against the void
out vec2  v_uv;
out float v_shade;
out vec2  v_stl;
flat out float v_layer;
flat out int v_tile;
out vec3  vWorldPos;
void main()
{
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    int idx  = int(a_stl.y) * u_lightmap_stride + int(a_stl.x);
    uint lm  = texelFetch(u_lightmap, ivec2(idx % 511, idx / 511), 0).r;
    // Negative a_shade: unlit (dig tag markers), drawn at that brightness.
    v_shade  = (a_shade < 0.0) ? -a_shade : max(float(lm) / 8192.0, u_min_shade) * a_shade;
    v_uv     = a_uv;
    v_stl    = a_stl;
    v_layer  = a_layer;
    v_tile   = int(a_tile + 0.5);
    vWorldPos = a_pos;
}
)glsl";

/* Things: upright sprites, lit by the lightmap like the walls and coloured
 * through the same palette and fade table. */
const char* const k_sprite_vs = R"glsl(
#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec2 a_stl;
uniform mat4       u_mvp;
uniform usampler2D u_lightmap;
uniform int        u_lightmap_stride;
uniform float      u_min_shade;
out vec2  v_uv;
out float v_shade;
void main()
{
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    if (a_stl.x < 0.0)
    {
        v_shade = 1.0;  // held in the hand: full brightness
    }
    else
    {
        int idx = int(a_stl.y) * u_lightmap_stride + int(a_stl.x);
        uint lm = texelFetch(u_lightmap, ivec2(idx % 511, idx / 511), 0).r;
        v_shade = max(float(lm) / 8192.0, u_min_shade);
    }
    v_uv    = a_uv;
}
)glsl";

const char* const k_sprite_fs = R"glsl(
#version 330 core
in vec2  v_uv;
in float v_shade;
uniform sampler2D u_sprite;      // RG8: palette index, coverage
uniform sampler2D u_palette;
uniform sampler2D u_fade_table;  // 256x64 palette fade rows
uniform int       u_use_fade;
uniform float     u_alpha;
uniform int       u_shadow;      // 1: soft round shadow, no texture
uniform float     u_one_colour;  // >= 0: fill covered pixels with this palette index (0..1)
out vec4 fragColor;
void main()
{
    if (u_shadow != 0)
    {
        float d = length(v_uv * 2.0 - 1.0);
        if (d >= 1.0)
            discard;
        fragColor = vec4(0.0, 0.0, 0.0, 0.5 * (1.0 - d * d));
        return;
    }
    vec2 ic = texture(u_sprite, v_uv).rg;
    if (ic.g < 0.5)
        discard;
    if (u_one_colour >= 0.0)
        ic.r = u_one_colour;
    vec3 col;
    if (u_use_fade != 0)
    {
        float row = clamp(v_shade * 32.0, 0.0, 63.0);
        float remapped = texture(u_fade_table, vec2(ic.r, (row + 0.5) / 64.0)).r;
        col = texture(u_palette, vec2(remapped, 0.5)).rgb;
    }
    else
    {
        col = texture(u_palette, vec2(ic.r, 0.5)).rgb * clamp(v_shade, 0.0, 1.0);
    }
    fragColor = vec4(col, u_alpha);
}
)glsl";

/* The wrist panel: a copy of the desktop side panel on a quad. */
const char* const k_panel_vs = R"glsl(
#version 330 core
layout(location = 0) in vec3 a_pos;   // unit cube corners; x/y in -0.5..0.5 used as the quad
uniform mat4 u_mvp;
out vec2 v_uv;
void main()
{
    gl_Position = u_mvp * vec4(a_pos.xy, 0.0, 1.0);
    v_uv = a_pos.xy + 0.5;
}
)glsl";

const char* const k_panel_fs = R"glsl(
#version 330 core
in vec2 v_uv;
uniform sampler2D u_panel;
out vec4 fragColor;
void main() { fragColor = vec4(texture(u_panel, v_uv).rgb, 1.0); }
)glsl";

/* Simple lit solid colour, for the Keeper's hand. */
const char* const k_lit_vs = R"glsl(
#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
uniform mat4 u_mvp;
uniform mat4 u_model;
out vec3 v_normal;
void main()
{
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_normal = mat3(transpose(inverse(u_model))) * a_normal;
}
)glsl";

const char* const k_lit_fs = R"glsl(
#version 330 core
in vec3 v_normal;
uniform vec4 u_color;
uniform vec3 u_light_dir;   // towards the light, reference space
out vec4 fragColor;
void main()
{
    vec3 n = normalize(v_normal);
    float diffuse = max(dot(n, u_light_dir), 0.0);
    float fill = 0.5 + 0.5 * n.y;   // soft light from above
    fragColor = vec4(u_color.rgb * (0.22 + 0.55 * diffuse + 0.23 * fill), 1.0);
}
)glsl";

const char* const k_solid_vs = R"glsl(
#version 330 core
layout(location = 0) in vec3 a_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); }
)glsl";

const char* const k_solid_fs = R"glsl(
#version 330 core
uniform vec4 u_color;
out vec4 fragColor;
void main() { fragColor = u_color; }
)glsl";

/******************************************************************************/
// Tuning

constexpr float k_near = 0.02f;
constexpr float k_far  = 50.0f;
/** Default size: one subtile is 1.2 cm, so a full 85x85 slab map is about 3 m wide. */
constexpr float k_default_scale = 0.012f / 256.0f;
constexpr float k_min_scale = 0.002f / 256.0f;
constexpr float k_max_scale = 0.25f / 256.0f;
constexpr float k_grab_on  = 0.60f;
constexpr float k_grab_off = 0.35f;

/******************************************************************************/
// State

struct EyeTarget {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int w = 0, h = 0;
    std::vector<XrSwapchainImageOpenGLKHR> images;
    GLuint fbo = 0;
    GLuint depth_rb = 0;
};

struct Chunk {
    GLuint vao = 0, vbo = 0;
    int count = 0;
};

struct Hand {
    bool tracked = false;
    bool grabbing = false;
    Vec3 pos = {0, 0, 0};
    Quat rot = {0, 0, 0, 1};
    float grip = 0;
    bool aim_tracked = false;        // pointing ray, -Z forward
    Vec3 aim_pos = {0, 0, 0};
    Quat aim_rot = {0, 0, 0, 1};
    float trigger = 0;
    bool action = false;             // A / X button
};

struct State {
    bool gl_ready = false;          // shaders and marker mesh created
    bool failed = false;            // gave up on VR for this run
    bool headset_missing = false;   // runtime is there but no headset yet
    ULONGLONG retry_at = 0;         // GetTickCount64() time of the next start attempt
    bool restart = false;           // runtime ended the session; start over

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    XrSpace space = XR_NULL_HANDLE;   // STAGE (floor level) when available, else LOCAL
    bool stage_space = false;
    XrSpace view_space = XR_NULL_HANDLE;
    EyeTarget eyes[2];

    XrActionSet action_set = XR_NULL_HANDLE;
    XrAction pose_action = XR_NULL_HANDLE;
    XrAction grip_action = XR_NULL_HANDLE;
    XrAction recenter_action = XR_NULL_HANDLE;
    XrAction aim_action = XR_NULL_HANDLE;
    XrAction select_action = XR_NULL_HANDLE;
    XrAction hand_action = XR_NULL_HANDLE;
    XrAction panel_toggle_action = XR_NULL_HANDLE;
    XrAction scroll_action = XR_NULL_HANDLE;    // left thumbstick
    float scroll_x = 0, scroll_y = 0;           // its current deflection
    Vec3 head_forward = {0, 0, -1};             // horizontal, from the last rendered frame
    bool ray_valid = false;                     // the pointing ray, out of the right palm
    Vec3 ray_origin = {0, 0, 0}, ray_dir = {0, 0, -1};
    XrPath hand_paths[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace hand_spaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aim_spaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    Hand hands[2];

    GLuint world_prog = 0;
    GLint loc_mvp = -1, loc_lightmap_stride = -1, loc_min_shade = -1;
    GLint loc_fullbright = -1, loc_ambient = -1, loc_shade_scale = -1, loc_shade_gamma = -1;
    GLint loc_lighting_mode = -1, loc_darkness_mode = -1, loc_tile_filter = -1, loc_missing_tile = -1;
    GLint loc_fog_speed = -1, loc_fog_density = -1, loc_time = -1;
    GLuint solid_prog = 0;
    GLint loc_solid_mvp = -1, loc_solid_color = -1;
    GLuint cube_vao = 0, cube_vbo = 0;
    GLuint lit_prog = 0;
    GLint loc_lit_mvp = -1, loc_lit_model = -1, loc_lit_color = -1, loc_lit_light = -1;
    GLuint lit_cube_vao = 0, lit_cube_vbo = 0;   // unit cube with face normals
    GLuint sprite_prog = 0;
    GLint loc_sprite_mvp = -1, loc_sprite_stride = -1, loc_sprite_min_shade = -1;
    GLint loc_sprite_use_fade = -1, loc_sprite_alpha = -1, loc_sprite_shadow = -1, loc_sprite_one_colour = -1;
    GLuint sprite_vao = 0, sprite_vbo = 0;
    std::unordered_map<int32_t, GLuint> sprite_textures;  // sprite id -> RG8 texture

    // Wrist panel: copy of the desktop side panel, shown above the left hand.
    GLuint panel_prog = 0;
    GLint loc_panel_mvp = -1;
    GLuint panel_tex = 0, panel_fbo = 0;
    int panel_w = 0, panel_h = 0;           // copied size in screen pixels
    bool panel_ready = false;               // copied from this frame's desktop image
    bool panel_shown = false;               // placed above the tracked left hand this frame
    bool panel_hidden = false;              // toggled with the left thumbstick click
    Vec3 panel_center = {0, 0, 0};
    Vec3 panel_right = {1, 0, 0}, panel_up = {0, 1, 0}, panel_normal = {0, 0, 1};
    float panel_world_w = 0, panel_world_h = 0;  // metres
    Vec3 panel_pointer = {0, 0, 0};         // where the right ray meets the panel

    std::vector<Chunk> chunks;
    int chunks_x = 0, chunks_y = 0;
    bool in_level = false;
    int lightmap_stride = 256;
    float focus_x = 0, focus_y = 0;              // where the desktop camera looks now
    float table_focus_x = 0, table_focus_y = 0;  // map point at the table centre, fixed when placed
    bool focus_is_heart = false;
    bool table_on_heart = false;                 // table_focus is the heart, not the stand-in
    int focus_lightness = 0;
    int frames_rendered = 0;

    // The dungeon tabletop, in the reference space.
    bool table_placed = false;
    Vec3 table_pos = {0, 0.9f, -0.6f};
    float table_yaw = 0;
    float table_scale = k_default_scale;

    // Grab gesture anchors, captured whenever the set of grabbing hands changes.
    int grab_mask = 0;
    Vec3 grab_hand[2];
    Vec3 grab_table_pos;
    float grab_yaw = 0, grab_scale = 0;
};

State s;

/** Written by the game thread in GameThreadSync() while the render thread is idle. */
DungeonSnapshot s_snapshot;
/** Things to draw, and the viewer position going the other way; same handoff as s_snapshot. */
ThingSnapshot s_things;

/** The right controller's ray and buttons, and where it hit; same handoff. */
PointerInput s_pointer_in;
PointerResult s_pointer_out;
PanelInfo s_panel_info;

/** Head position and gaze in game units (render -> game), for the sound listener. */
struct HeadInput {
    bool valid = false;
    float ox = 0, oy = 0, oz = 0;
    float dx = 0, dy = 0, dz = 0;
};
HeadInput s_head_in;

/** Sound listener, worked out and read on the game thread only. */
struct Listener {
    bool valid = false;
    int x = 0, y = 0, z = 0, angle = 0;
};
Listener s_listener;
bool s_select_held = false;   // trigger with hysteresis

struct SpriteVertex { float x, y, z, u, v, stl_x, stl_y; };
std::vector<SpriteVertex> s_sprite_verts;
/** Set by the render thread when VR could not start; the game thread then stops snapshotting. */
volatile bool s_given_up = false;

/******************************************************************************/
// Helpers

bool xr_ok(XrResult res, const char* what)
{
    if (XR_SUCCEEDED(res))
        return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (s.instance != XR_NULL_HANDLE && xrResultToString != nullptr)
        xrResultToString(s.instance, res, name);
    ERRORLOG("VR: %s failed (%d %s)", what, (int)res, name);
    return false;
}

XrPath to_path(const char* str)
{
    XrPath path = XR_NULL_PATH;
    xrStringToPath(s.instance, str, &path);
    return path;
}

Vec3 to_vec(const XrVector3f& v) { return { v.x, v.y, v.z }; }
Quat to_quat(const XrQuaternionf& q) { return { q.x, q.y, q.z, q.w }; }

GLuint compile_shader(GLenum type, const char* src, const char* name)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = "";
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        ERRORLOG("VR: %s shader failed to compile: %s", name, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint link_program(const char* vs_src, const char* fs_src, const char* name)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src, name);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src, name);
    if (vs == 0 || fs == 0)
    {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = "";
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        ERRORLOG("VR: %s program failed to link: %s", name, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/******************************************************************************/
// Setup

bool create_gl_resources()
{
    s.world_prog = link_program(k_world_vs, WORLD_FRAGMENT_SHADER, "VR world");
    s.solid_prog = link_program(k_solid_vs, k_solid_fs, "VR solid");
    s.sprite_prog = link_program(k_sprite_vs, k_sprite_fs, "VR sprite");
    s.panel_prog = link_program(k_panel_vs, k_panel_fs, "VR panel");
    s.lit_prog = link_program(k_lit_vs, k_lit_fs, "VR lit");
    if (s.world_prog == 0 || s.solid_prog == 0 || s.sprite_prog == 0 || s.panel_prog == 0 || s.lit_prog == 0)
        return false;

    s.loc_lit_mvp   = glGetUniformLocation(s.lit_prog, "u_mvp");
    s.loc_lit_model = glGetUniformLocation(s.lit_prog, "u_model");
    s.loc_lit_color = glGetUniformLocation(s.lit_prog, "u_color");
    s.loc_lit_light = glGetUniformLocation(s.lit_prog, "u_light_dir");
    {
        // Unit cube centred on the origin, position + face normal per vertex.
        std::vector<float> cube;
        const float faces[6][3] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };
        for (const auto& n : faces)
        {
            // Two axes spanning the face (culling is off, so winding does not matter).
            const float u[3] = { n[0] == 0 ? 1.0f : 0.0f, n[0] != 0 ? 1.0f : 0.0f, 0.0f };
            const float v[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };
            const float corners[6][2] = { {-1, -1}, {1, -1}, {1, 1}, {1, 1}, {-1, 1}, {-1, -1} };
            for (const auto& c : corners)
            {
                for (int k = 0; k < 3; k++)
                    cube.push_back(0.5f * n[k] + 0.5f * c[0] * u[k] + 0.5f * c[1] * v[k]);
                for (int k = 0; k < 3; k++)
                    cube.push_back(n[k]);
            }
        }
        glGenVertexArrays(1, &s.lit_cube_vao);
        glGenBuffers(1, &s.lit_cube_vbo);
        glBindVertexArray(s.lit_cube_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s.lit_cube_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cube.size() * sizeof(float)), cube.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glUseProgram(s.panel_prog);
    glUniform1i(glGetUniformLocation(s.panel_prog, "u_panel"), 0);
    s.loc_panel_mvp = glGetUniformLocation(s.panel_prog, "u_mvp");

    glUseProgram(s.sprite_prog);
    glUniform1i(glGetUniformLocation(s.sprite_prog, "u_sprite"), 0);
    glUniform1i(glGetUniformLocation(s.sprite_prog, "u_palette"), 1);
    glUniform1i(glGetUniformLocation(s.sprite_prog, "u_lightmap"), 2);
    glUniform1i(glGetUniformLocation(s.sprite_prog, "u_fade_table"), 3);
    s.loc_sprite_mvp       = glGetUniformLocation(s.sprite_prog, "u_mvp");
    s.loc_sprite_stride    = glGetUniformLocation(s.sprite_prog, "u_lightmap_stride");
    s.loc_sprite_min_shade = glGetUniformLocation(s.sprite_prog, "u_min_shade");
    s.loc_sprite_use_fade  = glGetUniformLocation(s.sprite_prog, "u_use_fade");
    s.loc_sprite_alpha     = glGetUniformLocation(s.sprite_prog, "u_alpha");
    s.loc_sprite_shadow    = glGetUniformLocation(s.sprite_prog, "u_shadow");
    s.loc_sprite_one_colour = glGetUniformLocation(s.sprite_prog, "u_one_colour");

    glGenVertexArrays(1, &s.sprite_vao);
    glGenBuffers(1, &s.sprite_vbo);
    glBindVertexArray(s.sprite_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s.sprite_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SpriteVertex), (void*)offsetof(SpriteVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVertex), (void*)offsetof(SpriteVertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVertex), (void*)offsetof(SpriteVertex, stl_x));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_things.reset_cache = true;  // no sprite textures yet: have the game thread send them all
    {
        // One covered pixel; filled quads take their colour from one_colour.
        const uint8_t texel[2] = { 0, 255 };
        GLuint& tex = s.sprite_textures[k_solid_sprite_id];
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, 1, 1, 0, GL_RG, GL_UNSIGNED_BYTE, texel);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glUseProgram(s.world_prog);
    glUniform1i(glGetUniformLocation(s.world_prog, "u_tile_atlas"), 0);
    glUniform1i(glGetUniformLocation(s.world_prog, "u_palette"), 1);
    glUniform1i(glGetUniformLocation(s.world_prog, "u_lightmap"), 2);
    glUniform1i(glGetUniformLocation(s.world_prog, "u_fade_table"), 3);
    s.loc_mvp             = glGetUniformLocation(s.world_prog, "u_mvp");
    s.loc_lightmap_stride = glGetUniformLocation(s.world_prog, "u_lightmap_stride");
    s.loc_min_shade       = glGetUniformLocation(s.world_prog, "u_min_shade");
    s.loc_fullbright      = glGetUniformLocation(s.world_prog, "u_fullbright");
    s.loc_ambient         = glGetUniformLocation(s.world_prog, "u_ambient");
    s.loc_shade_scale     = glGetUniformLocation(s.world_prog, "u_shade_scale");
    s.loc_shade_gamma     = glGetUniformLocation(s.world_prog, "u_shade_gamma");
    s.loc_lighting_mode   = glGetUniformLocation(s.world_prog, "u_lighting_mode");
    s.loc_darkness_mode   = glGetUniformLocation(s.world_prog, "u_darkness_mode");
    s.loc_tile_filter     = glGetUniformLocation(s.world_prog, "u_tile_filter");
    s.loc_missing_tile    = glGetUniformLocation(s.world_prog, "u_missing_tile");
    s.loc_fog_speed       = glGetUniformLocation(s.world_prog, "u_fog_speed");
    s.loc_fog_density     = glGetUniformLocation(s.world_prog, "u_fog_density");
    s.loc_time            = glGetUniformLocation(s.world_prog, "u_time");

    glUseProgram(s.solid_prog);
    s.loc_solid_mvp   = glGetUniformLocation(s.solid_prog, "u_mvp");
    s.loc_solid_color = glGetUniformLocation(s.solid_prog, "u_color");
    glUseProgram(0);

    // Unit cube centred on the origin, used for the controller markers.
    static const float k_cube[] = {
        -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f,-0.5f, 0.5f,
        -0.5f,-0.5f,-0.5f, -0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f,-0.5f,-0.5f, -0.5f,-0.5f,-0.5f,
        -0.5f, 0.5f,-0.5f, -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f,-0.5f, -0.5f, 0.5f,-0.5f,
        -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f, -0.5f,-0.5f, 0.5f, -0.5f,-0.5f,-0.5f,
         0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f,-0.5f,-0.5f,
        -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f,-0.5f, -0.5f,-0.5f,-0.5f,
    };
    glGenVertexArrays(1, &s.cube_vao);
    glGenBuffers(1, &s.cube_vbo);
    glBindVertexArray(s.cube_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s.cube_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_cube), k_cube, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

bool create_swapchains()
{
    uint32_t count = 0;
    xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    if (count != 2)
    {
        ERRORLOG("VR: expected 2 stereo views, runtime reports %u", count);
        return false;
    }
    XrViewConfigurationView views[2] = { {XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW} };
    if (!xr_ok(xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                 2, &count, views), "xrEnumerateViewConfigurationViews"))
        return false;

    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(s.session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(s.session, format_count, &format_count, formats.data());
    // The palette colours are sRGB values. Written unconverted into an sRGB
    // swapchain (GL_FRAMEBUFFER_SRGB off), the compositor reads them correctly.
    int64_t format = 0;
    for (int64_t f : formats)
        if (f == GL_SRGB8_ALPHA8) format = f;
    if (format == 0)
        for (int64_t f : formats)
            if (f == GL_RGBA8) format = f;
    if (format == 0 && !formats.empty())
        format = formats[0];
    if (format == 0)
    {
        ERRORLOG("VR: runtime offers no swapchain formats");
        return false;
    }

    for (int i = 0; i < 2; i++)
    {
        EyeTarget& eye = s.eyes[i];
        eye.w = (int)views[i].recommendedImageRectWidth;
        eye.h = (int)views[i].recommendedImageRectHeight;
        XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = (uint32_t)eye.w;
        ci.height = (uint32_t)eye.h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!xr_ok(xrCreateSwapchain(s.session, &ci, &eye.swapchain), "xrCreateSwapchain"))
            return false;
        uint32_t image_count = 0;
        xrEnumerateSwapchainImages(eye.swapchain, 0, &image_count, nullptr);
        XrSwapchainImageOpenGLKHR blank = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR};
        eye.images.assign(image_count, blank);
        if (!xr_ok(xrEnumerateSwapchainImages(eye.swapchain, image_count, &image_count,
                       (XrSwapchainImageBaseHeader*)eye.images.data()), "xrEnumerateSwapchainImages"))
            return false;

        glGenFramebuffers(1, &eye.fbo);
        glGenRenderbuffers(1, &eye.depth_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, eye.depth_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eye.w, eye.h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    return true;
}

bool create_actions()
{
    XrActionSetCreateInfo set_info = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(set_info.actionSetName, "keeper");
    std::strcpy(set_info.localizedActionSetName, "Keeper");
    if (!xr_ok(xrCreateActionSet(s.instance, &set_info, &s.action_set), "xrCreateActionSet"))
        return false;

    s.hand_paths[0] = to_path("/user/hand/left");
    s.hand_paths[1] = to_path("/user/hand/right");

    auto make_action = [](XrActionType type, const char* name, const char* label, bool per_hand, XrAction* out) {
        XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = type;
        std::strcpy(info.actionName, name);
        std::strcpy(info.localizedActionName, label);
        if (per_hand)
        {
            info.countSubactionPaths = 2;
            info.subactionPaths = s.hand_paths;
        }
        return xr_ok(xrCreateAction(s.action_set, &info, out), name);
    };
    if (!make_action(XR_ACTION_TYPE_POSE_INPUT, "hand_pose", "Hand pose", true, &s.pose_action) ||
        !make_action(XR_ACTION_TYPE_FLOAT_INPUT, "grab_table", "Grab the dungeon", true, &s.grip_action) ||
        !make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "recenter", "Recenter the dungeon", false, &s.recenter_action) ||
        !make_action(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Pointing ray", true, &s.aim_action) ||
        !make_action(XR_ACTION_TYPE_FLOAT_INPUT, "select", "Pick up / dig (left click)", true, &s.select_action) ||
        !make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "hand_action", "Drop / slap (right click)", true, &s.hand_action) ||
        !make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "toggle_panel", "Show / hide the wrist panel", false, &s.panel_toggle_action) ||
        !make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "scroll", "Scroll the dungeon", false, &s.scroll_action))
        return false;

    const char* const profiles[] = {
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/valve/index_controller",
    };
    for (const char* profile : profiles)
    {
        const XrActionSuggestedBinding bindings[] = {
            {s.pose_action,     to_path("/user/hand/left/input/grip/pose")},
            {s.pose_action,     to_path("/user/hand/right/input/grip/pose")},
            {s.grip_action,     to_path("/user/hand/left/input/squeeze/value")},
            {s.grip_action,     to_path("/user/hand/right/input/squeeze/value")},
            {s.recenter_action, to_path("/user/hand/right/input/thumbstick/click")},
            {s.aim_action,      to_path("/user/hand/left/input/aim/pose")},
            {s.aim_action,      to_path("/user/hand/right/input/aim/pose")},
            {s.select_action,   to_path("/user/hand/left/input/trigger/value")},
            {s.select_action,   to_path("/user/hand/right/input/trigger/value")},
            {s.hand_action,     to_path("/user/hand/right/input/a/click")},
            {s.panel_toggle_action, to_path("/user/hand/left/input/thumbstick/click")},
            {s.scroll_action,   to_path("/user/hand/left/input/thumbstick")},
        };
        XrInteractionProfileSuggestedBinding suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = to_path(profile);
        suggested.suggestedBindings = bindings;
        suggested.countSuggestedBindings = (uint32_t)(sizeof(bindings) / sizeof(bindings[0]));
        xr_ok(xrSuggestInteractionProfileBindings(s.instance, &suggested), profile);
    }

    XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &s.action_set;
    if (!xr_ok(xrAttachSessionActionSets(s.session, &attach), "xrAttachSessionActionSets"))
        return false;

    for (int i = 0; i < 2; i++)
    {
        XrActionSpaceCreateInfo info = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        info.action = s.pose_action;
        info.subactionPath = s.hand_paths[i];
        info.poseInActionSpace.orientation.w = 1.0f;
        if (!xr_ok(xrCreateActionSpace(s.session, &info, &s.hand_spaces[i]), "xrCreateActionSpace"))
            return false;
        info.action = s.aim_action;
        if (!xr_ok(xrCreateActionSpace(s.session, &info, &s.aim_spaces[i]), "xrCreateActionSpace(aim)"))
            return false;
    }
    return true;
}

bool xr_init()
{
    if (!load_runtime())
        return false;

    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ici.applicationInfo.applicationName, "KeeperFX VR");
    ici.applicationInfo.applicationVersion = 1;
    std::strcpy(ici.applicationInfo.engineName, "KeeperFX");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    const char* const extensions[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = extensions;
    if (!xr_ok(xrCreateInstance(&ici, &s.instance), "xrCreateInstance (does the runtime support OpenGL?)"))
        return false;
    if (!load_instance_functions(s.instance))
        return false;

    XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
    if (!s.headset_missing && XR_SUCCEEDED(xrGetInstanceProperties(s.instance, &props)))
        SYNCLOG("VR: OpenXR runtime \"%s\"", props.runtimeName);  // once, not on every retry

    XrSystemGetInfo sys_info = {XR_TYPE_SYSTEM_GET_INFO};
    sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult sys_res = xrGetSystem(s.instance, &sys_info, &s.system);
    if (sys_res == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
    {
        // Headset not connected yet (e.g. Virtual Desktop not streaming): retried later.
        if (!s.headset_missing)
            SYNCLOG("VR: waiting for the headset to connect");
        s.headset_missing = true;
        return false;
    }
    if (!xr_ok(sys_res, "xrGetSystem"))
        return false;
    s.headset_missing = false;

    // Required by XR_KHR_opengl_enable before creating the session.
    PFN_xrGetOpenGLGraphicsRequirementsKHR get_requirements = nullptr;
    xrGetInstanceProcAddr(s.instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&get_requirements);
    XrGraphicsRequirementsOpenGLKHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if (get_requirements == nullptr ||
        !xr_ok(get_requirements(s.instance, s.system, &requirements), "xrGetOpenGLGraphicsRequirementsKHR"))
        return false;

    XrGraphicsBindingOpenGLWin32KHR binding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    binding.hDC = wglGetCurrentDC();
    binding.hGLRC = wglGetCurrentContext();
    if (binding.hDC == nullptr || binding.hGLRC == nullptr)
    {
        ERRORLOG("VR: no current WGL context on the render thread");
        return false;
    }
    XrSessionCreateInfo session_info = {XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = s.system;
    if (!xr_ok(xrCreateSession(s.instance, &session_info, &s.session), "xrCreateSession"))
        return false;

    XrReferenceSpaceCreateInfo space_info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    s.stage_space = XR_SUCCEEDED(xrCreateReferenceSpace(s.session, &space_info, &s.space));
    if (!s.stage_space)
    {
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!xr_ok(xrCreateReferenceSpace(s.session, &space_info, &s.space), "xrCreateReferenceSpace"))
            return false;
    }
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!xr_ok(xrCreateReferenceSpace(s.session, &space_info, &s.view_space), "xrCreateReferenceSpace(VIEW)"))
        return false;

    if (!create_swapchains())
        return false;
    if (!create_actions())
        WARNLOG("VR: controller input unavailable");

    SYNCLOG("VR: session created, %s space, %dx%d per eye", s.stage_space ? "stage" : "local",
            s.eyes[0].w, s.eyes[0].h);
    return true;
}

/** Ends the OpenXR side (instance, session, swapchains, actions). The
 *  dungeon mesh and shaders stay, so a later retry can reuse them. */
void destroy_xr()
{
    for (EyeTarget& eye : s.eyes)
    {
        if (eye.fbo) glDeleteFramebuffers(1, &eye.fbo);
        if (eye.depth_rb) glDeleteRenderbuffers(1, &eye.depth_rb);
        eye = EyeTarget();
    }
    // Destroying the instance destroys the session, spaces, swapchains and actions with it.
    if (s.instance != XR_NULL_HANDLE && xrDestroyInstance != nullptr)
        xrDestroyInstance(s.instance);
    s.instance = XR_NULL_HANDLE;
    s.system = XR_NULL_SYSTEM_ID;
    s.session = XR_NULL_HANDLE;
    s.session_state = XR_SESSION_STATE_UNKNOWN;
    s.running = false;
    s.space = s.view_space = XR_NULL_HANDLE;
    s.action_set = XR_NULL_HANDLE;
    s.pose_action = s.grip_action = s.recenter_action = XR_NULL_HANDLE;
    s.aim_action = s.select_action = s.hand_action = s.panel_toggle_action = XR_NULL_HANDLE;
    s.scroll_action = XR_NULL_HANDLE;
    s.scroll_x = s.scroll_y = 0.0f;
    s.hand_spaces[0] = s.hand_spaces[1] = XR_NULL_HANDLE;
    s.aim_spaces[0] = s.aim_spaces[1] = XR_NULL_HANDLE;
    for (Hand& hand : s.hands)
        hand = Hand();
    s.grab_mask = 0;
}

void destroy_gl()
{
    for (Chunk& c : s.chunks)
    {
        if (c.vbo) glDeleteBuffers(1, &c.vbo);
        if (c.vao) glDeleteVertexArrays(1, &c.vao);
    }
    s.chunks.clear();
    s.chunks_x = s.chunks_y = 0;
    if (s.cube_vbo) glDeleteBuffers(1, &s.cube_vbo);
    if (s.cube_vao) glDeleteVertexArrays(1, &s.cube_vao);
    if (s.world_prog) glDeleteProgram(s.world_prog);
    if (s.solid_prog) glDeleteProgram(s.solid_prog);
    s.cube_vbo = s.cube_vao = s.world_prog = s.solid_prog = 0;
    for (auto& entry : s.sprite_textures)
        glDeleteTextures(1, &entry.second);
    s.sprite_textures.clear();
    if (s.sprite_vbo) glDeleteBuffers(1, &s.sprite_vbo);
    if (s.sprite_vao) glDeleteVertexArrays(1, &s.sprite_vao);
    if (s.sprite_prog) glDeleteProgram(s.sprite_prog);
    s.sprite_vbo = s.sprite_vao = s.sprite_prog = 0;
    if (s.panel_fbo) glDeleteFramebuffers(1, &s.panel_fbo);
    if (s.panel_tex) glDeleteTextures(1, &s.panel_tex);
    if (s.panel_prog) glDeleteProgram(s.panel_prog);
    if (s.lit_cube_vbo) glDeleteBuffers(1, &s.lit_cube_vbo);
    if (s.lit_cube_vao) glDeleteVertexArrays(1, &s.lit_cube_vao);
    if (s.lit_prog) glDeleteProgram(s.lit_prog);
    s.lit_cube_vbo = s.lit_cube_vao = s.lit_prog = 0;
    s.panel_fbo = s.panel_tex = s.panel_prog = 0;
    s.panel_w = s.panel_h = 0;
    s.panel_ready = false;
}

/** Copies the side panel out of the finished desktop frame into panel_tex. */
void capture_panel(GLuint screen_fbo, int screen_w, int screen_h)
{
    s.panel_ready = false;
    if (!s_panel_info.visible || screen_fbo == 0 || screen_w <= 0 || screen_h <= 0)
        return;
    const int w = (int)(s_panel_info.width_frac * (float)screen_w + 0.5f);
    const int h = screen_h;
    if (w <= 0)
        return;
    if (s.panel_tex == 0 || w != s.panel_w || h != s.panel_h)
    {
        if (s.panel_tex == 0) glGenTextures(1, &s.panel_tex);
        if (s.panel_fbo == 0) glGenFramebuffers(1, &s.panel_fbo);
        glBindTexture(GL_TEXTURE_2D, s.panel_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s.panel_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.panel_tex, 0);
        s.panel_w = w;
        s.panel_h = h;
    }
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, screen_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.panel_fbo);
    // The panel sits at the left edge of the screen, full height.
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, screen_fbo);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    s.panel_ready = true;
}

/** Floats the panel above the left controller, turned to face the head. */
bool place_panel(Vec3 head)
{
    const Hand& left = s.hands[0];
    if (!s.panel_ready || !left.tracked || s.panel_h <= 0)
        return false;
    constexpr float k_panel_height = 0.30f;  // metres
    s.panel_world_h = k_panel_height;
    s.panel_world_w = k_panel_height * (float)s.panel_w / (float)s.panel_h;
    s.panel_center = left.pos + Vec3{0.0f, 0.06f + k_panel_height * 0.5f, 0.0f};
    Vec3 n = head - s.panel_center;
    const float len = Length(n);
    if (len < 0.01f)
        return false;
    n = n * (1.0f / len);
    // right = up x n, up' = n x right (right-handed, Y up)
    Vec3 right = { n.z, 0.0f, -n.x };
    const float rlen = Length(right);
    if (rlen < 0.01f)
        return false;
    right = right * (1.0f / rlen);
    const Vec3 up = { n.y * right.z - n.z * right.y, n.z * right.x - n.x * right.z, n.x * right.y - n.y * right.x };
    s.panel_normal = n;
    s.panel_right = right;
    s.panel_up = up;
    return true;
}

/** Ray against the wrist panel; on a hit returns the panel coordinates (0..1, v up). */
bool ray_hits_panel(Vec3 origin, Vec3 dir, float* u, float* v, Vec3* point)
{
    const float denom = dir.x * s.panel_normal.x + dir.y * s.panel_normal.y + dir.z * s.panel_normal.z;
    if (std::fabs(denom) < 1e-5f)
        return false;
    const Vec3 to_center = s.panel_center - origin;
    const float t = (to_center.x * s.panel_normal.x + to_center.y * s.panel_normal.y + to_center.z * s.panel_normal.z) / denom;
    if (t <= 0.0f)
        return false;
    const Vec3 p = origin + dir * t;
    const Vec3 d = p - s.panel_center;
    const float pu = (d.x * s.panel_right.x + d.y * s.panel_right.y + d.z * s.panel_right.z) / s.panel_world_w + 0.5f;
    const float pv = (d.x * s.panel_up.x + d.y * s.panel_up.y + d.z * s.panel_up.z) / s.panel_world_h + 0.5f;
    if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f)
        return false;
    *u = pu;
    *v = pv;
    *point = p;
    return true;
}

Mat4 panel_model()
{
    Mat4 m = Mat4::Identity();
    const Vec3 r = s.panel_right * s.panel_world_w, u = s.panel_up * s.panel_world_h;
    m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
    m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
    m.m[8] = s.panel_normal.x; m.m[9] = s.panel_normal.y; m.m[10] = s.panel_normal.z;
    m.m[12] = s.panel_center.x; m.m[13] = s.panel_center.y; m.m[14] = s.panel_center.z;
    return m;
}

/** Uploads sprite images the game thread decoded. */
void apply_sprite_images()
{
    if (s_things.new_images.empty())
        return;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (const SpriteImage& img : s_things.new_images)
    {
        GLuint& tex = s.sprite_textures[img.id];
        if (tex == 0)
            glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, img.w, img.h, 0, GL_RG, GL_UNSIGNED_BYTE, img.rg.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    s_things.new_images.clear();
}

/******************************************************************************/
// Per frame

/** Uploads the chunks the game thread rebuilt. */
void apply_snapshot()
{
    DungeonSnapshot& snap = s_snapshot;
    s.in_level = snap.in_level;
    if (!snap.in_level)
        return;
    s.lightmap_stride = snap.lightmap_stride;
    s.focus_x = snap.focus_x;
    s.focus_y = snap.focus_y;
    s.focus_lightness = snap.focus_lightness;
    s.focus_is_heart = snap.focus_is_heart;
    // Placed before the heart existed: place again now that it does.
    if (s.table_placed && !s.table_on_heart && s.focus_is_heart)
        s.table_placed = false;

    if (snap.chunks_x != s.chunks_x || snap.chunks_y != s.chunks_y)
    {
        for (Chunk& c : s.chunks)
        {
            if (c.vbo) glDeleteBuffers(1, &c.vbo);
            if (c.vao) glDeleteVertexArrays(1, &c.vao);
        }
        s.chunks_x = snap.chunks_x;
        s.chunks_y = snap.chunks_y;
        s.chunks.assign((size_t)(s.chunks_x * s.chunks_y), Chunk());
        s.table_placed = false;  // new map: put it in front of the player
    }

    for (const ChunkUpdate& update : snap.updates)
    {
        if (update.index < 0 || update.index >= (int)s.chunks.size())
            continue;
        Chunk& c = s.chunks[(size_t)update.index];
        if (c.vao == 0)
        {
            glGenVertexArrays(1, &c.vao);
            glGenBuffers(1, &c.vbo);
            glBindVertexArray(c.vao);
            glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
            const GLsizei stride = (GLsizei)sizeof(DungeonVertex);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(DungeonVertex, x));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(DungeonVertex, u));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(DungeonVertex, shade));
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(DungeonVertex, stl_x));
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(DungeonVertex, layer));
            glEnableVertexAttribArray(5);
            glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(DungeonVertex, tile));
            glBindVertexArray(0);
        }
        glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(update.verts.size() * sizeof(DungeonVertex)),
                     update.verts.empty() ? nullptr : update.verts.data(), GL_STATIC_DRAW);
        c.count = (int)update.verts.size();
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    snap.updates.clear();
}

void poll_events()
{
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(s.instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const XrEventDataSessionStateChanged* changed = (const XrEventDataSessionStateChanged*)&event;
            s.session_state = changed->state;
            switch (changed->state)
            {
            case XR_SESSION_STATE_READY:
            {
                XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                s.running = xr_ok(xrBeginSession(s.session, &begin), "xrBeginSession");
                if (s.running)
                    SYNCLOG("VR: session running");
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(s.session);
                s.running = false;
                SYNCLOG("VR: session stopped");
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                s.running = false;
                s.restart = true;
                SYNCLOG("VR: session ended by the runtime, will reconnect");
                break;
            default:
                break;
            }
        }
        else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        {
            s.running = false;
            s.restart = true;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

/** Dungeon tabletop: game units -> reference space. The focus point sits at
 *  table_pos; game x runs right, game y (south) towards the player, game z up. */
Mat4 table_model()
{
    Mat4 swizzle = Mat4::Identity();
    swizzle.m[5] = 0.0f; swizzle.m[6] = 1.0f;   // game y -> +Z
    swizzle.m[9] = 1.0f; swizzle.m[10] = 0.0f;  // game z -> +Y
    return Mat4::Translation(s.table_pos) * Mat4::RotationY(s.table_yaw)
         * Mat4::Scale({s.table_scale, s.table_scale, s.table_scale}) * swizzle
         * Mat4::Translation({-s.table_focus_x, -s.table_focus_y, 0.0f});
}

/** Puts the dungeon about half a metre in front of the head and a bit below
 *  it, facing the player. */
void place_table(XrTime time)
{
    XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(s.view_space, s.space, time, &head)))
        return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((head.locationFlags & need) != need)
        return;
    Vec3 forward = Rotate(to_quat(head.pose.orientation), {0, 0, -1});
    forward.y = 0;
    const float len = Length(forward);
    forward = (len > 0.01f) ? forward * (1.0f / len) : Vec3{0, 0, -1};
    s.table_pos = to_vec(head.pose.position) + forward * 0.55f;
    s.table_pos.y -= 0.45f;
    s.table_yaw = Heading(forward * -1.0f);
    s.table_scale = k_default_scale;
    s.table_focus_x = s.focus_x;
    s.table_focus_y = s.focus_y;
    s.table_on_heart = s.focus_is_heart;
    s.grab_mask = -1;  // re-anchor any grab in progress
    s.table_placed = true;
}

void update_table_grab()
{
    int mask = 0;
    for (int i = 0; i < 2; i++)
    {
        Hand& hand = s.hands[i];
        if (!hand.tracked)
            hand.grabbing = false;
        else if (hand.grabbing ? (hand.grip < k_grab_off) : (hand.grip > k_grab_on))
            hand.grabbing = !hand.grabbing;
        if (hand.grabbing)
            mask |= 1 << i;
    }
    if (mask != s.grab_mask)
    {
        s.grab_mask = mask;
        s.grab_hand[0] = s.hands[0].pos;
        s.grab_hand[1] = s.hands[1].pos;
        s.grab_table_pos = s.table_pos;
        s.grab_yaw = s.table_yaw;
        s.grab_scale = s.table_scale;
        return;
    }
    if (mask == 3)
    {
        // Two hands: scale by the change in distance, rotate by the change in
        // heading, and keep the point between the hands fixed to the dungeon.
        Vec3 d0 = s.grab_hand[1] - s.grab_hand[0];
        Vec3 d1 = s.hands[1].pos - s.hands[0].pos;
        d0.y = 0;
        d1.y = 0;
        const float l0 = Length(d0), l1 = Length(d1);
        if (l0 < 0.02f || l1 < 0.02f)
            return;
        float scale = s.grab_scale * (l1 / l0);
        if (scale < k_min_scale) scale = k_min_scale;
        if (scale > k_max_scale) scale = k_max_scale;
        const float ratio = scale / s.grab_scale;
        const float turn = Heading(d1) - Heading(d0);
        const Vec3 mid0 = (s.grab_hand[0] + s.grab_hand[1]) * 0.5f;
        const Vec3 mid1 = (s.hands[0].pos + s.hands[1].pos) * 0.5f;
        s.table_scale = scale;
        s.table_yaw = s.grab_yaw + turn;
        s.table_pos = mid1 + RotateY(s.grab_table_pos - mid0, turn) * ratio;
    }
    else if (mask != 0)
    {
        const int i = (mask == 1) ? 0 : 1;
        s.table_pos = s.grab_table_pos + (s.hands[i].pos - s.grab_hand[i]);
    }
}

/** Left thumbstick: slides the dungeon under you, like scrolling the camera.
 *  Push forward to move forward over the map; the response is gentle near
 *  the centre and speeds up towards full deflection. */
void update_table_scroll(float dt)
{
    if (s.grab_mask != 0 || dt <= 0.0f)
        return;  // a grab is moving the table already
    constexpr float k_deadzone = 0.15f;
    constexpr float k_speed = 0.6f;  // metres per second at the default size, full deflection
    const float mag = std::sqrt(s.scroll_x * s.scroll_x + s.scroll_y * s.scroll_y);
    if (mag <= k_deadzone)
        return;
    const float eased = (mag - k_deadzone) / (1.0f - k_deadzone);
    const float gain = eased * eased / mag;
    // Faster when the table is bigger, so the dungeon passes at the same rate.
    const float speed = k_speed * (s.table_scale / k_default_scale) * dt;
    const Vec3 fwd = s.head_forward;
    const Vec3 right = { -fwd.z, 0.0f, fwd.x };
    const Vec3 move = (fwd * (s.scroll_y * gain) + right * (s.scroll_x * gain)) * speed;
    s.table_pos = s.table_pos - move;
}

void update_input(XrTime time, float dt)
{
    for (Hand& hand : s.hands)
    {
        hand.tracked = false;
        hand.aim_tracked = false;
        hand.trigger = 0.0f;
        hand.action = false;
    }
    s.scroll_x = s.scroll_y = 0.0f;
    if (s.action_set == XR_NULL_HANDLE || s.session_state != XR_SESSION_STATE_FOCUSED)
    {
        update_table_grab();
        return;
    }
    XrActiveActionSet active = {s.action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(s.session, &sync)))
        return;

    for (int i = 0; i < 2; i++)
    {
        Hand& hand = s.hands[i];
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(s.hand_spaces[i], s.space, time, &loc)) && (loc.locationFlags & need) == need)
        {
            hand.tracked = true;
            hand.pos = to_vec(loc.pose.position);
            hand.rot = to_quat(loc.pose.orientation);
        }
        XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = s.grip_action;
        get.subactionPath = s.hand_paths[i];
        XrActionStateFloat grip = {XR_TYPE_ACTION_STATE_FLOAT};
        hand.grip = (XR_SUCCEEDED(xrGetActionStateFloat(s.session, &get, &grip)) && grip.isActive) ? grip.currentState : 0.0f;

        XrSpaceLocation aim = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(s.aim_spaces[i], s.space, time, &aim)) && (aim.locationFlags & need) == need)
        {
            hand.aim_tracked = true;
            hand.aim_pos = to_vec(aim.pose.position);
            hand.aim_rot = to_quat(aim.pose.orientation);
        }
        get.action = s.select_action;
        XrActionStateFloat trigger = {XR_TYPE_ACTION_STATE_FLOAT};
        hand.trigger = (XR_SUCCEEDED(xrGetActionStateFloat(s.session, &get, &trigger)) && trigger.isActive) ? trigger.currentState : 0.0f;
        get.action = s.hand_action;
        XrActionStateBoolean button = {XR_TYPE_ACTION_STATE_BOOLEAN};
        hand.action = XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &get, &button)) && button.isActive && button.currentState;
    }

    XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = s.recenter_action;
    XrActionStateBoolean recenter = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &get, &recenter)) &&
        recenter.isActive && recenter.changedSinceLastSync && recenter.currentState)
        s.table_placed = false;

    get.action = s.panel_toggle_action;
    XrActionStateBoolean toggle = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &get, &toggle)) &&
        toggle.isActive && toggle.changedSinceLastSync && toggle.currentState)
        s.panel_hidden = !s.panel_hidden;

    get.action = s.scroll_action;
    XrActionStateVector2f stick = {XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(s.session, &get, &stick)) && stick.isActive)
    {
        s.scroll_x = stick.currentState.x;
        s.scroll_y = stick.currentState.y;
    }

    update_table_grab();
    update_table_scroll(dt);
}

/** Game units -> reference space; same as table_model() applied to a point. */
Vec3 game_to_stage(Vec3 g)
{
    const Vec3 local = { (g.x - s.table_focus_x) * s.table_scale, g.z * s.table_scale, (g.y - s.table_focus_y) * s.table_scale };
    return RotateY(local, s.table_yaw) + s.table_pos;
}

/** Reference space -> game units; inverse of table_model(). */
Vec3 stage_to_game(Vec3 p)
{
    const Vec3 q = RotateY(p - s.table_pos, -s.table_yaw) * (1.0f / s.table_scale);
    return { q.x + s.table_focus_x, q.z + s.table_focus_y, q.y };
}

/** Builds this frame's sprite quads, each turned to face @p viewer (game units)
 *  about the vertical axis. Quads are in s_things.sprites order, 6 vertices each. */
void build_sprite_vertices(Vec3 viewer)
{
    s_sprite_verts.clear();
    // Held things hang just below the right controller, a bit enlarged so
    // they read well up close.
    const bool hand_tracked = s.hands[1].tracked;
    const Vec3 hand = stage_to_game(s.hands[1].pos + Vec3{0.0f, -0.04f, 0.0f});
    constexpr float k_held_scale = 1.5f;
    for (const SpriteBillboard& placed : s_things.sprites)
    {
        SpriteBillboard b = placed;
        if (b.in_hand)
        {
            b.left *= k_held_scale;
            b.right *= k_held_scale;
            b.top *= k_held_scale;
            b.bottom *= k_held_scale;
            b.x = hand.x;
            b.y = hand.y;
            b.z = hand.z - b.top;  // top of the sprite at the hand
        }
        float fx = viewer.x - b.x, fy = viewer.y - b.y;
        const float len = std::sqrt(fx * fx + fy * fy);
        if (len > 1.0f) { fx /= len; fy /= len; }
        else { fx = 0.0f; fy = 1.0f; }
        // Right as seen by the viewer, who looks along -f.
        const float rx = fy, ry = -fx;
        if (b.in_hand)
        {
            if (!hand_tracked)
            {
                // Keep the 6-vertices-per-sprite indexing, draw nothing.
                const SpriteVertex none = {0, 0, 0, 0, 0, 0, 0};
                s_sprite_verts.insert(s_sprite_verts.end(), 6, none);
                continue;
            }
            // Fan several held things out sideways, the next one to drop in front.
            const float spread = (b.right - b.left) * 0.7f * (float)b.hand_slot;
            b.x += rx * spread - fx * 8.0f * (float)b.hand_slot;
            b.y += ry * spread - fy * 8.0f * (float)b.hand_slot;
        }
        if (b.kind == SK_Shadow)
        {
            // Flat on the floor, an ellipse along the map axes.
            const SpriteVertex a = { b.x + b.left,  b.y + b.bottom, b.z, 0.0f, 0.0f, -1.0f, -1.0f };
            const SpriteVertex c = { b.x + b.right, b.y + b.bottom, b.z, 1.0f, 0.0f, -1.0f, -1.0f };
            const SpriteVertex d = { b.x + b.right, b.y + b.top,    b.z, 1.0f, 1.0f, -1.0f, -1.0f };
            const SpriteVertex e = { b.x + b.left,  b.y + b.top,    b.z, 0.0f, 1.0f, -1.0f, -1.0f };
            s_sprite_verts.push_back(a); s_sprite_verts.push_back(c); s_sprite_verts.push_back(d);
            s_sprite_verts.push_back(d); s_sprite_verts.push_back(e); s_sprite_verts.push_back(a);
            continue;
        }
        // Layered icons sit slightly in front of each other.
        b.x += fx * b.lift;
        b.y += fy * b.lift;
        const float u0 = b.flip ? 1.0f : 0.0f, u1 = 1.0f - u0;
        const SpriteVertex tl = { b.x + rx * b.left,  b.y + ry * b.left,  b.z + b.top,    u0, 0.0f, b.stl_x, b.stl_y };
        const SpriteVertex tr = { b.x + rx * b.right, b.y + ry * b.right, b.z + b.top,    u1, 0.0f, b.stl_x, b.stl_y };
        const SpriteVertex br = { b.x + rx * b.right, b.y + ry * b.right, b.z + b.bottom, u1, 1.0f, b.stl_x, b.stl_y };
        const SpriteVertex bl = { b.x + rx * b.left,  b.y + ry * b.left,  b.z + b.bottom, u0, 1.0f, b.stl_x, b.stl_y };
        s_sprite_verts.push_back(tl); s_sprite_verts.push_back(tr); s_sprite_verts.push_back(br);
        s_sprite_verts.push_back(br); s_sprite_verts.push_back(bl); s_sprite_verts.push_back(tl);
    }
    glBindBuffer(GL_ARRAY_BUFFER, s.sprite_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_sprite_verts.size() * sizeof(SpriteVertex)),
                 s_sprite_verts.empty() ? nullptr : s_sprite_verts.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/** Draws the sprites built by build_sprite_vertices(). Expects the palette,
 *  lightmap and fade table bound on units 1-3, as for the walls. */
void draw_sprites(const Mat4& mvp, const GLWorldViewRenderer::VRTextures& tex)
{
    if (s_things.sprites.empty())
        return;
    glUseProgram(s.sprite_prog);
    glUniformMatrix4fv(s.loc_sprite_mvp, 1, GL_FALSE, mvp.m);
    glUniform1i(s.loc_sprite_stride, s.lightmap_stride);
    glUniform1f(s.loc_sprite_min_shade, 0.40f);
    glUniform1i(s.loc_sprite_use_fade, tex.fade != 0 ? 1 : 0);
    glBindVertexArray(s.sprite_vao);
    glActiveTexture(GL_TEXTURE0);

    // Opaque sprites first, then the floor shadows, then half transparent
    // (ghosts, own creatures' thought clouds), then additive (glows). All but
    // the first leave depth alone so they never hide what is behind them.
    enum { PassOpaque, PassShadows, PassHalf, PassAdditive, PassCount };
    for (int pass = 0; pass < PassCount; pass++)
    {
        if (pass == PassOpaque)
        {
            glDisable(GL_BLEND);
            glUniform1f(s.loc_sprite_alpha, 1.0f);
        }
        else
        {
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            if (pass == PassAdditive)
                glBlendFunc(GL_ONE, GL_ONE);
            else
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUniform1f(s.loc_sprite_alpha, pass == PassHalf ? 0.5f : 1.0f);
        }
        glUniform1i(s.loc_sprite_shadow, pass == PassShadows ? 1 : 0);
        for (size_t i = 0; i < s_things.sprites.size(); i++)
        {
            const SpriteBillboard& b = s_things.sprites[i];
            if (pass == PassShadows)
            {
                if (b.kind != SK_Shadow)
                    continue;
                glDrawArrays(GL_TRIANGLES, (GLint)(i * 6), 6);
                continue;
            }
            if (b.kind != SK_Upright)
                continue;
            const int wanted = (b.blend == 0) ? PassOpaque : (b.blend == 1) ? PassHalf : PassAdditive;
            if (wanted != pass)
                continue;
            const auto it = s.sprite_textures.find(b.sprite_id);
            if (it == s.sprite_textures.end())
                continue;
            glUniform1f(s.loc_sprite_one_colour, b.one_colour >= 0 ? (float)b.one_colour / 255.0f : -1.0f);
            glBindTexture(GL_TEXTURE_2D, it->second);
            glDrawArrays(GL_TRIANGLES, (GLint)(i * 6), 6);
        }
    }
    glUniform1i(s.loc_sprite_shadow, 0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}

void draw_lit_box(const Mat4& view_proj, const Mat4& model, float r, float g, float b)
{
    const Mat4 mvp = view_proj * model;
    glUniformMatrix4fv(s.loc_lit_mvp, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(s.loc_lit_model, 1, GL_FALSE, model.m);
    glUniform4f(s.loc_lit_color, r, g, b, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 36);
}

/** The Keeper's hand, built from boxes at the controller's grip position.
 *  It is turned like the aim pose (the laser): -Z forward along the
 *  controller, +Y up, and for the right hand the palm faces -X. The grip
 *  pose's own axes do not follow the controller that way on every runtime.
 *  The index finger curls with the trigger, the other fingers and thumb with
 *  the grip, so squeezing makes a fist. */
void draw_keeper_hand(const Mat4& view_proj, const Hand& hand)
{
    // Pale, slightly sickly skin, dark claws, a crimson sleeve.
    constexpr float skin[3]   = { 0.80f, 0.68f, 0.58f };
    constexpr float claw[3]   = { 0.22f, 0.10f, 0.08f };
    constexpr float sleeve[3] = { 0.45f, 0.08f, 0.10f };
    const float index_curl = hand.trigger;
    const float fist_curl = hand.grip;

    glUseProgram(s.lit_prog);
    const float light_len = std::sqrt(0.3f * 0.3f + 0.9f * 0.9f + 0.4f * 0.4f);
    glUniform3f(s.loc_lit_light, 0.3f / light_len, 0.9f / light_len, 0.4f / light_len);
    glBindVertexArray(s.lit_cube_vao);

    const Mat4 grip = Mat4::FromPose(hand.aim_tracked ? hand.aim_rot : hand.rot, hand.pos);
    // Back of the hand sits just outside the controller handle.
    const Mat4 palm = grip * Mat4::Translation({0.034f, 0.0f, 0.004f});
    draw_lit_box(view_proj, palm * Mat4::Scale({0.022f, 0.074f, 0.084f}), skin[0], skin[1], skin[2]);
    draw_lit_box(view_proj, palm * Mat4::Translation({0.002f, 0.0f, 0.062f}) * Mat4::Scale({0.034f, 0.064f, 0.05f}),
                 sleeve[0], sleeve[1], sleeve[2]);

    // Fingers, index on top (+Y) down to the little finger.
    const float finger_y[4] = { 0.027f, 0.009f, -0.009f, -0.026f };
    const float finger_len[4] = { 0.95f, 1.0f, 0.93f, 0.76f };
    const float segment_len[3] = { 0.040f, 0.026f, 0.020f };
    const float joint_bend[3] = { 1.25f, 1.45f, 1.0f };   // radians at a full curl
    constexpr float k_rest_curl = 0.08f;                  // a relaxed, nearly straight hand
    for (int f = 0; f < 4; f++)
    {
        const float curl = (f == 0) ? index_curl : fist_curl;
        Mat4 m = palm * Mat4::Translation({-0.002f, finger_y[f], -0.042f});
        for (int j = 0; j < 3; j++)
        {
            const float len = segment_len[j] * finger_len[f];
            m = m * Mat4::RotationY(joint_bend[j] * curl + k_rest_curl);
            const float width = 0.017f - 0.002f * (float)j;
            draw_lit_box(view_proj, m * Mat4::Translation({0.0f, 0.0f, -len * 0.5f}) * Mat4::Scale({width, width, len}),
                         skin[0], skin[1], skin[2]);
            m = m * Mat4::Translation({0.0f, 0.0f, -len});
        }
        // A short claw past the fingertip, bending on towards the palm.
        const Mat4 tip = m * Mat4::RotationY(0.5f);
        draw_lit_box(view_proj, tip * Mat4::Translation({0.0f, 0.0f, -0.006f}) * Mat4::Scale({0.008f, 0.01f, 0.014f}),
                     claw[0], claw[1], claw[2]);
    }

    // Thumb: from the top of the palm, along the controller's face.
    Mat4 thumb = palm * Mat4::Translation({-0.010f, 0.038f, -0.006f})
               * Mat4::RotationY(0.45f + 0.55f * fist_curl) * Mat4::RotationX(0.25f);
    const float thumb_len[2] = { 0.032f, 0.026f };
    for (int j = 0; j < 2; j++)
    {
        draw_lit_box(view_proj, thumb * Mat4::Translation({0.0f, 0.0f, -thumb_len[j] * 0.5f}) * Mat4::Scale({0.019f, 0.019f, thumb_len[j]}),
                     skin[0], skin[1], skin[2]);
        thumb = thumb * Mat4::Translation({0.0f, 0.0f, -thumb_len[j]}) * Mat4::RotationY(0.35f * fist_curl + 0.1f);
    }
    draw_lit_box(view_proj, thumb * Mat4::Translation({0.0f, 0.0f, -0.006f}) * Mat4::Scale({0.009f, 0.011f, 0.013f}),
                 claw[0], claw[1], claw[2]);

    glBindVertexArray(0);
}

void draw_eye(const EyeTarget& eye, const Mat4& view_proj, const GLWorldViewRenderer::VRTextures& tex)
{
    glViewport(0, 0, eye.w, eye.h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    if (s.in_level && tex.palette != 0 && tex.lightmap != 0)
    {
        const Mat4 mvp = view_proj * table_model();
        glUseProgram(s.world_prog);
        glUniformMatrix4fv(s.loc_mvp, 1, GL_FALSE, mvp.m);
        glUniform1i(s.loc_lightmap_stride, s.lightmap_stride);
        glUniform1f(s.loc_min_shade, 0.40f);  // tabletop stays readable even where the game is dark
        glUniform1f(s.loc_fullbright, g_renderer_settings.shade_fullbright);
        glUniform1f(s.loc_ambient, g_renderer_settings.shade_ambient);
        glUniform1f(s.loc_shade_scale, g_renderer_settings.shade_scale);
        glUniform1f(s.loc_shade_gamma, g_renderer_settings.shade_gamma);
        glUniform1i(s.loc_lighting_mode, 0);
        // Without the fade table the palette-LUT path would sample nothing and turn black.
        glUniform1i(s.loc_darkness_mode, tex.fade != 0 ? g_renderer_settings.darkness_mode : 0);
        glUniform1i(s.loc_tile_filter, g_renderer_settings.tile_filter);
        glUniform1f(s.loc_missing_tile, tex.atlas == 0 ? 1.0f : 0.0f);
        glUniform1f(s.loc_fog_speed, g_renderer_settings.fog_speed);
        glUniform1f(s.loc_fog_density, g_renderer_settings.fog_density);
        glUniform1f(s.loc_time, 0.0f);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex.atlas);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex.palette);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, tex.lightmap);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, tex.fade);

        for (const Chunk& c : s.chunks)
        {
            if (c.count == 0)
                continue;
            glBindVertexArray(c.vao);
            glDrawArrays(GL_TRIANGLES, 0, c.count);
        }
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

        draw_sprites(mvp, tex);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }

    // The Keeper's hand on the right controller.
    if (s.hands[1].tracked)
        draw_keeper_hand(view_proj, s.hands[1]);

    // Left controller marker: a small block, green while grabbing.
    glUseProgram(s.solid_prog);
    glBindVertexArray(s.cube_vao);
    for (const Hand& hand : s.hands)
    {
        if (!hand.tracked || &hand == &s.hands[1])
            continue;
        const Mat4 mvp = view_proj * Mat4::FromPose(hand.rot, hand.pos) * Mat4::Scale({0.03f, 0.03f, 0.09f});
        glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, mvp.m);
        if (hand.grabbing)
            glUniform4f(s.loc_solid_color, 0.35f, 0.85f, 0.35f, 1.0f);
        else
            glUniform4f(s.loc_solid_color, 0.75f, 0.70f, 0.60f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }

    // Wrist panel.
    if (s.panel_shown)
    {
        glUseProgram(s.panel_prog);
        const Mat4 mvp = view_proj * panel_model();
        glUniformMatrix4fv(s.loc_panel_mvp, 1, GL_FALSE, mvp.m);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s.panel_tex);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(s.solid_prog);
    }

    // Pointing ray from the right controller, ending in a marker where it meets
    // the dungeon or the panel. Brighter while the trigger is held.
    const Hand& pointer = s.hands[1];
    if (s_pointer_in.valid && s.ray_valid)
    {
        float length = 2.0f;
        Vec3 hit_stage = {0, 0, 0};
        const bool ends = s_pointer_in.panel_hit || s_pointer_out.hit;
        if (s_pointer_in.panel_hit)
            hit_stage = s.panel_pointer;
        else if (s_pointer_out.hit)
            hit_stage = game_to_stage({s_pointer_out.x, s_pointer_out.y, s_pointer_out.z});
        if (ends)
            length = Length(hit_stage - s.ray_origin);
        const float glow = s_pointer_in.select ? 1.0f : 0.6f;
        // Along the hand's -X (out of the palm), see render_views().
        const Mat4 ray = view_proj * Mat4::FromPose(pointer.aim_rot, s.ray_origin)
                       * Mat4::Translation({-length * 0.5f, 0, 0}) * Mat4::Scale({length, 0.003f, 0.003f});
        glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, ray.m);
        glUniform4f(s.loc_solid_color, 0.9f * glow, 0.25f * glow, 0.15f * glow, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        if (ends)
        {
            const float size = s_pointer_in.panel_hit ? 0.006f : 0.012f;
            const Mat4 dot = view_proj * Mat4::Translation(hit_stage) * Mat4::Scale({size, size, size});
            glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, dot.m);
            glUniform4f(s.loc_solid_color, 1.0f, 0.85f * glow, 0.3f * glow, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
    }
    glBindVertexArray(0);
    glUseProgram(0);
}

bool render_views(GLWorldViewRenderer& world, XrTime time,
                  XrCompositionLayerProjection& layer, XrCompositionLayerProjectionView* proj_views)
{
    XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = time;
    locate.space = s.space;
    XrViewState view_state = {XR_TYPE_VIEW_STATE};
    XrView views[2] = { {XR_TYPE_VIEW}, {XR_TYPE_VIEW} };
    uint32_t view_count = 0;
    if (XR_FAILED(xrLocateViews(s.session, &locate, &view_state, 2, &view_count, views)) || view_count != 2)
        return false;
    const XrViewStateFlags need = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if ((view_state.viewStateFlags & need) != need)
        return false;

    if (!s.table_placed && s.in_level)
        place_table(time);

    // Sprites face the point between the eyes; the game thread also uses it
    // next frame to pick each creature's facing.
    const Vec3 head = (to_vec(views[0].pose.position) + to_vec(views[1].pose.position)) * 0.5f;
    const Vec3 viewer = stage_to_game(head);
    s_things.viewer_x = viewer.x;
    s_things.viewer_y = viewer.y;
    s_things.viewer_valid = true;
    build_sprite_vertices(viewer);

    // Horizontal facing, for thumbstick scrolling.
    const Vec3 gaze = Rotate(to_quat(views[0].pose.orientation), {0, 0, -1});
    const float flat = std::sqrt(gaze.x * gaze.x + gaze.z * gaze.z);
    if (flat > 0.1f)
        s.head_forward = { gaze.x / flat, 0.0f, gaze.z / flat };

    // Head gaze for the sound listener.
    const Vec3 gaze_ahead = stage_to_game(head + gaze);
    s_head_in.valid = true;
    s_head_in.ox = viewer.x; s_head_in.oy = viewer.y; s_head_in.oz = viewer.z;
    s_head_in.dx = gaze_ahead.x - viewer.x; s_head_in.dy = gaze_ahead.y - viewer.y; s_head_in.dz = gaze_ahead.z - viewer.z;

    // The right controller's pointing ray, in game units, for the game thread.
    const Hand& pointer = s.hands[1];
    PointerInput in;
    // The ray comes straight out of the palm of the Keeper's hand (turned like
    // the aim pose, palm towards -X), so a palm-down hand points straight down
    // at what is under it, without bending the wrist.
    s.ray_valid = pointer.aim_tracked && pointer.tracked;
    if (s.ray_valid)
    {
        s.ray_origin = pointer.pos + Rotate(pointer.aim_rot, {0.022f, 0.0f, 0.004f});
        s.ray_dir = Rotate(pointer.aim_rot, {-1.0f, 0.0f, 0.0f});
    }
    s.panel_shown = !s.panel_hidden && s.in_level && place_panel(head);
    if (s.ray_valid && s.panel_shown)
        in.panel_hit = ray_hits_panel(s.ray_origin, s.ray_dir, &in.panel_u, &in.panel_v, &s.panel_pointer);
    if (s.ray_valid && s.in_level)
    {
        const Vec3 origin = stage_to_game(s.ray_origin);
        const Vec3 ahead = stage_to_game(s.ray_origin + s.ray_dir);
        in.valid = true;
        in.ox = origin.x; in.oy = origin.y; in.oz = origin.z;
        in.dx = ahead.x - origin.x; in.dy = ahead.y - origin.y; in.dz = ahead.z - origin.z;
        if (s_select_held ? (pointer.trigger < 0.35f) : (pointer.trigger > 0.6f))
            s_select_held = !s_select_held;
        in.select = s_select_held;
        in.action = pointer.action;
    }
    else
    {
        s_select_held = false;
    }
    s_pointer_in = in;

    const GLWorldViewRenderer::VRTextures tex = world.GetVRTextures();
    if ((s.frames_rendered++ % 450) == 0)
    {
        int chunks = 0, verts = 0;
        for (const Chunk& c : s.chunks)
        {
            if (c.count > 0) chunks++;
            verts += c.count;
        }
        SYNCLOG("VR: in_level %d placed %d, %d chunks / %d verts, %d sprites / %d images, tex atlas %u pal %u light %u fade %u, "
                "table (%.2f %.2f %.2f) scale %.6f, focus (%.0f %.0f) lightness %d, head (%.2f %.2f %.2f)",
                (int)s.in_level, (int)s.table_placed, chunks, verts, (int)s_things.sprites.size(), (int)s.sprite_textures.size(), tex.atlas, tex.palette, tex.lightmap, tex.fade,
                s.table_pos.x, s.table_pos.y, s.table_pos.z, s.table_scale * 256.0f, s.focus_x, s.focus_y,
                s.focus_lightness, views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z);
    }
    for (int i = 0; i < 2; i++)
    {
        EyeTarget& eye = s.eyes[i];
        uint32_t image_index = 0;
        XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!xr_ok(xrAcquireSwapchainImage(eye.swapchain, &acquire, &image_index), "xrAcquireSwapchainImage"))
            return false;
        XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (!xr_ok(xrWaitSwapchainImage(eye.swapchain, &wait), "xrWaitSwapchainImage"))
            return false;

        glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye.images[image_index].image, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, eye.depth_rb);

        const XrFovf& fov = views[i].fov;
        const Mat4 proj = Mat4::Projection(fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown, k_near, k_far);
        const Mat4 view = Mat4::FromPose(to_quat(views[i].pose.orientation), to_vec(views[i].pose.position)).InverseRigid();
        draw_eye(eye, proj * view, tex);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(eye.swapchain, &release);

        proj_views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        proj_views[i].pose = views[i].pose;
        proj_views[i].fov = views[i].fov;
        proj_views[i].subImage.swapchain = eye.swapchain;
        proj_views[i].subImage.imageRect.offset = {0, 0};
        proj_views[i].subImage.imageRect.extent = {eye.w, eye.h};
        proj_views[i].subImage.imageArrayIndex = 0;
    }
    glDisable(GL_DEPTH_TEST);

    layer.space = s.space;
    layer.viewCount = 2;
    layer.views = proj_views;
    return true;
}

} // namespace

/******************************************************************************/

void GameThreadSync()
{
    if (!vr_requested || s_given_up)
        return;

    // Headset audio usually appears as a new Windows default device once the
    // headset connects; keep the sound effects following it.
    static ULONGLONG next_audio_check = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= next_audio_check)
    {
        next_audio_check = now + 3000;
        FollowDefaultAudioDevice();
    }

    UpdateDungeonSnapshot(s_snapshot);
    if (s_snapshot.in_level)
    {
        UpdateThingSnapshot(s_things);
        ApplyPointer(s_pointer_in, &s_pointer_out);
        GetPanelInfo(&s_panel_info);

        // Listen where the head looks at the table; straight below the head
        // if the gaze misses the map.
        s_listener.valid = false;
        if (s_head_in.valid)
        {
            MapHit hit;
            if (RaycastMap(s_head_in.ox, s_head_in.oy, s_head_in.oz, s_head_in.dx, s_head_in.dy, s_head_in.dz, &hit))
            {
                s_listener.x = (int)hit.x;
                s_listener.y = (int)hit.y;
                s_listener.z = (int)hit.z;
            }
            else
            {
                const float max_x = s_snapshot.map_w - 1.0f, max_y = s_snapshot.map_h - 1.0f;
                s_listener.x = (int)std::fmax(0.0f, std::fmin(s_head_in.ox, max_x));
                s_listener.y = (int)std::fmax(0.0f, std::fmin(s_head_in.oy, max_y));
                s_listener.z = 0;
            }
            s_listener.angle = (int)(LbArcTanAngle((int32_t)s_head_in.dx, (int32_t)s_head_in.dy) & ANGLE_MASK);
            s_listener.valid = true;
        }
    }
    else
    {
        s_things.sprites.clear();
        s_pointer_out.hit = false;
        s_listener.valid = false;
    }
}

void RenderThreadFrame(GLWorldViewRenderer& world, unsigned int screen_fbo, int screen_w, int screen_h)
{
    if (!vr_requested || s.failed)
        return;
    s_head_in.valid = false;  // set again below if this frame reaches the headset
    if (!s.gl_ready)
    {
        if (!create_gl_resources())
        {
            ERRORLOG("VR: could not create the VR shaders, continuing on the desktop only");
            destroy_gl();
            s.failed = true;
            s_given_up = true;
            return;
        }
        s.gl_ready = true;
    }

    // The mesh is kept up to date even before the headset connects.
    apply_snapshot();
    apply_sprite_images();

    if (s.session == XR_NULL_HANDLE)
    {
        const ULONGLONG now = GetTickCount64();
        if (now < s.retry_at)
            return;
        if (!xr_init())
        {
            destroy_xr();
            if (!s.headset_missing)
            {
                ERRORLOG("VR: could not start VR, continuing on the desktop only");
                destroy_gl();
                s.failed = true;
                s_given_up = true;
                return;
            }
            s.retry_at = now + 3000;
            return;
        }
    }

    poll_events();
    if (s.restart)
    {
        s.restart = false;
        destroy_xr();
        s.retry_at = GetTickCount64() + 3000;
        return;
    }
    if (!s.running)
        return;
    capture_panel((GLuint)screen_fbo, screen_w, screen_h);

    XrFrameWaitInfo wait_info = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state = {XR_TYPE_FRAME_STATE};
    if (!xr_ok(xrWaitFrame(s.session, &wait_info, &frame_state), "xrWaitFrame"))
        return;
    XrFrameBeginInfo begin_info = {XR_TYPE_FRAME_BEGIN_INFO};
    if (!xr_ok(xrBeginFrame(s.session, &begin_info), "xrBeginFrame"))
        return;

    // predictedDisplayPeriod is the frame time in nanoseconds; capped so a stall does not jump the table.
    const float dt = std::fmin((float)frame_state.predictedDisplayPeriod * 1e-9f, 0.05f);
    update_input(frame_state.predictedDisplayTime, dt);

    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView proj_views[2];
    bool have_layer = false;
    if (frame_state.shouldRender)
        have_layer = render_views(world, frame_state.predictedDisplayTime, layer, proj_views);

    const XrCompositionLayerBaseHeader* layers[1] = { (const XrCompositionLayerBaseHeader*)&layer };
    XrFrameEndInfo end_info = {XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = have_layer ? 1 : 0;
    end_info.layers = layers;
    xr_ok(xrEndFrame(s.session, &end_info), "xrEndFrame");
}

bool IsSessionRunning()
{
    return s.running;
}

void RenderThreadShutdown()
{
    if (s.session != XR_NULL_HANDLE && s.running)
        xrEndSession(s.session);
    destroy_xr();
    destroy_gl();
    s.gl_ready = false;
}

} // namespace VR

int VR_GetSoundListener(int* x, int* y, int* z, int* angle)
{
    if (!vr_requested || !VR::s_listener.valid)
        return 0;
    *x = VR::s_listener.x;
    *y = VR::s_listener.y;
    *z = VR::s_listener.z;
    *angle = VR::s_listener.angle;
    return 1;
}

int VR_IsHeadsetActive(void)
{
    // The listener is worked out each frame the headset renders the dungeon.
    return (vr_requested && VR::s_listener.valid) ? 1 : 0;
}

#else // !KFX_VR

namespace VR {
void GameThreadSync() {}
void RenderThreadFrame(GLWorldViewRenderer&, unsigned int, int, int) {}
bool IsSessionRunning() { return false; }
void RenderThreadShutdown() {}
} // namespace VR

int VR_GetSoundListener(int*, int*, int*, int*) { return 0; }
int VR_IsHeadsetActive(void) { return 0; }

#include "post_inc.h"

#endif // KFX_VR
