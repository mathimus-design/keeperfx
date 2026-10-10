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
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrApplyHapticFeedback)

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
/** Default size: one subtile is 3 cm (a slab 9 cm), so the rooms around the
 *  Dungeon Heart fill the view; grip with both hands to see more of the map. */
constexpr float k_default_scale = 0.03f / 256.0f;
/** Possession: the dungeon at life size around the creature, 70 cm a subtile. */
constexpr float k_fp_scale = 0.7f / 256.0f;
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
    XrAction turn_action = XR_NULL_HANDLE;      // right thumbstick
    XrAction haptic_action = XR_NULL_HANDLE;    // controller vibration, per hand
    XrAction pause_action = XR_NULL_HANDLE;     // left menu button
    float turn_x = 0;                           // its sideways deflection
    Vec3 head_forward = {0, 0, -1};             // horizontal, from the last rendered frame
    // Possession: life size, with the creature's eyes where the player's head
    // was when it began, turned so the creature faced the way the player did.
    bool possessing = false;
    bool fp_reanchor = false;                   // recenter pressed while possessing
    Vec3 fp_anchor = {0, 0, 0};
    float fp_yaw = 0;
    Vec3 fp_eye = {0, 0, 0};                    // game units
    Vec3 last_head = {0, 0, 0};                 // head position of the last rendered frame
    int window_w = 0, window_h = 0;             // desktop window, for the spectator mirror
    bool finish_was_down = false;               // A / trigger last frame, to continue after a level
    float decided_time = 0;                     // seconds since the level was won or lost
    float frame_dt = 0;                         // seconds, this frame
    // Pickaxe swing detection (possessing a digger).
    bool swing_have_last = false;
    Vec3 swing_last_head = {0, 0, 0};
    float swing_cooldown = 0;
    int swing_click_frames = 0;
    bool ray_valid = false;                     // the pointing ray from the right hand
    bool ray_forward = false;                   // forward along the controller, else out of the palm
    bool ray_left = false;                      // from the left hand, casting the chosen spell
    float ray_palm_sign = -1.0f;                // palm ray along the hand's -X (right) or +X (left)
    bool possess_was_chosen = false;            // Possess was chosen last frame
    bool panel_auto_hidden = false;             // the panel folded away by itself (a tool picked from it)
    float panel_click_time = 0;                 // seconds since a press on the panel still counting
    int last_work_state = 0;                    // to see the player state change
    Quat ray_rot = {0, 0, 0, 1};                // the ray hand's aim orientation
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
    std::unordered_map<int32_t, std::pair<int, int>> sprite_sizes;  // sprite id -> width, height
    // Palm menu: creature icons over the upturned left palm (stage space).
    GLuint ui_vao = 0, ui_vbo = 0;
    bool palm_shown = false;
    bool palm_armed = true;                 // a tap counts again once the finger has left the icons
    int palm_touched = -1;                  // icon under the right fingertip
    int palm_hover = -1;                    // icon the fingertip is just in front of
    int palm_flash = -1;                    // icon just tapped, flashing
    float palm_flash_time = 0;              // seconds of flash left
    float point_pose = 0;                   // 0..1: right hand easing into a pointing pose
    // Slap gesture: the right palm's last position, and a pause between swats.
    bool slap_have_last = false;
    Vec3 slap_last = {0, 0, 0};
    float slap_cooldown = 0;
    Vec3 palm_right = {1, 0, 0}, palm_up = {0, 1, 0}, palm_normal = {0, 0, 1};

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
    Vec3 panel_pointer = {0, 0, 0};         // where the right ray (or fingertip) meets the panel
    // Message box copy (objective / info text), beside the wrist panel.
    GLuint info_tex = 0, info_fbo = 0;
    int info_w = 0, info_h = 0;             // copied size in screen pixels
    bool info_ready = false;                // copied this frame
    bool info_shown = false;                // drawn this frame
    bool info_hidden = false;               // hidden with the button on the panel
    // "Back to the hand" button, shown while a room / spell / trap... is chosen.
    bool cancel_shown = false;
    bool cancel_hover = false;
    bool cancel_armed = true;
    float cancel_flash = 0;
    Vec3 cancel_center = {0, 0, 0};
    Vec3 cancel_right = {1, 0, 0}, cancel_up = {0, 1, 0}, cancel_normal = {0, 0, 1};
    Vec3 info_center = {0, 0, 0};
    Vec3 info_right = {1, 0, 0}, info_up = {0, 1, 0}, info_normal = {0, 0, 1};
    float info_world_w = 0, info_world_h = 0;
    bool info_finger = false;               // fingertip (or ray) on it
    bool info_touching = false;
    bool info_was_pressed = false;
    float info_flash_time = 0;
    Vec3 info_pointer = {0, 0, 0};
    // New text alert: the left hand glows yellow and buzzes until the text is looked at.
    bool text_alert = false;
    bool text_was_visible = false;
    int text_last_message = 0;
    int text_alert_pulses = 0;
    float text_alert_timer = 0;
    // The box's own buttons (tick, eye...), repeated in a row under it.
    struct CapturedRect { GLuint tex = 0, fbo = 0; int w = 0, h = 0; };
    CapturedRect info_btn[6];
    int info_btn_count = 0;                 // copied this frame
    Vec3 info_btn_center[6];
    float info_btn_world_w[6] = {}, info_btn_world_h[6] = {};
    int info_btn_hover = -1;                // button the fingertip is on
    int info_btn_flash = -1;                // button just pressed
    // The hide / show button above the panel's top-right corner.
    bool info_button_shown = false;
    Vec3 info_button_center = {0, 0, 0};
    bool info_button_armed = true;
    bool info_button_hover = false;
    float info_button_flash = 0;
    bool panel_finger = false;              // the index fingertip is on the panel
    bool panel_touching = false;            // ... and pushed into it
    bool panel_was_pressed = false;         // last frame, to catch a new press
    float panel_flash_time = 0;             // seconds of the press flash left

    // Menu screen: the whole desktop screen, floating within reach while a
    // menu is up, touched with the index finger.
    GLuint menu_tex = 0, menu_fbo = 0;
    int menu_w = 0, menu_h = 0;
    bool menu_ready = false;                // copied this frame
    bool menu_shown = false;                // drawn this frame
    bool menu_placed = false;               // placed since it last appeared
    Vec3 menu_center = {0, 0, 0};
    Vec3 menu_right = {1, 0, 0}, menu_up = {0, 1, 0}, menu_normal = {0, 0, 1};
    float menu_world_w = 0, menu_world_h = 0;
    bool menu_touching = false;             // finger pushed into the screen
    bool menu_pointer_valid = false;        // finger or ray over the screen
    Vec3 menu_pointer = {0, 0, 0};          // where, for the marker
    bool menu_by_ray = false;               // the ray rather than the finger
    bool menu_was_pressed = false;          // last frame's press, to catch a new one
    float menu_flash_time = 0;              // seconds of the press flash left

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

/** The floating menu screen: whether the game wants it (game -> render) and
 *  the touch on it (render -> game). */
bool s_menu_wanted = false;
MenuInput s_menu_in;

/** The message box: where it is (game -> render) and a touch on it (render -> game). */
InfoBoxInfo s_info_box;
MenuInput s_info_in;

/** A spell is chosen and waiting to be cast, and whether it is Possess (game -> render). */
bool s_spell_chosen = false;
bool s_possess_chosen = false;
/** The player's state and whether it is the plain hand (game -> render). */
int s_work_state = 0;
bool s_plain_hand = true;

/** The pause button was pressed (render -> game), and the game thread's copy
 *  that the input code takes once. */
bool s_pause_request = false;
bool s_pause_pending = false;

/** The level is won or lost (game -> render); A or the trigger then asks to
 *  continue (render -> game), taken once by the input code. */
bool s_level_decided = false;
bool s_finish_request = false;
/** The "back to the hand" button was pressed (render -> game). */
bool s_cancel_request = false;
bool s_finish_pending = false;

/** Slapping with the hand: where the right palm is over the map and whether
 *  it just swatted (render -> game); the target picked, for the input code,
 *  and that one landed, for the buzz (game -> render). */
struct SlapInput {
    bool low = false;            // palm low over the table
    bool swipe = false;          // fast swat this frame
    float x = 0, y = 0;          // palm over the map, game units
    float radius = 0;            // how far from the palm a creature counts
};
SlapInput s_slap_in;
int s_slap_pending = 0;
bool s_slap_landed = false;

/** Creature type to pick up from the palm menu (render -> game), 0 for none. */
int s_palm_pick_kind = 0;

/** Head position and gaze in game units (render -> game), for the sound listener. */
struct HeadInput {
    bool valid = false;
    float ox = 0, oy = 0, oz = 0;
    float dx = 0, dy = 0, dz = 0;
};
HeadInput s_head_in;

/** The possessed creature (game -> render). */
PossessionView s_possession;

/** What the headset asks of the possessed creature (render -> game), and the
 *  game thread's own copy that the input code reads between frames. */
struct PossessionLook {
    bool valid = false;
    int yaw = 0, pitch = 0;
    int move_x = 0, move_y = 0;
};
PossessionLook s_look;
PossessionLook s_look_game;

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
    // Same layout for sprites placed in the reference space (the palm menu).
    glGenVertexArrays(1, &s.ui_vao);
    glGenBuffers(1, &s.ui_vbo);
    glBindVertexArray(s.ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s.ui_vbo);
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
        !make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "scroll", "Scroll the dungeon", false, &s.scroll_action) ||
        !make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Turn (possessing)", false, &s.turn_action) ||
        !make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Feedback buzz", true, &s.haptic_action) ||
        !make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "pause", "Pause the game", false, &s.pause_action))
        return false;

    const char* const profiles[] = {
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/valve/index_controller",
    };
    // Pause: the left menu button and Y on Touch controllers (SteamVR keeps the
    // menu button for its dashboard, so Y always works); Index ones have no
    // menu button, so their left B and A. Paths differ per controller.
    const char* const pause_paths[][2] = {
        { "/user/hand/left/input/menu/click", "/user/hand/left/input/y/click" },
        { "/user/hand/left/input/b/click",    "/user/hand/left/input/a/click" },
    };
    // Grip: the Index's squeeze value follows the fingers resting on the grip,
    // so just holding the controller reads as gripping; its force sensor needs
    // an actual squeeze.
    const char* const grip_paths[][2] = {
        { "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value" },
        { "/user/hand/left/input/squeeze/force", "/user/hand/right/input/squeeze/force" },
    };
    for (size_t p = 0; p < sizeof(profiles) / sizeof(profiles[0]); p++)
    {
        const char* const profile = profiles[p];
        const XrActionSuggestedBinding bindings[] = {
            {s.pause_action,    to_path(pause_paths[p][0])},
            {s.pause_action,    to_path(pause_paths[p][1])},
            {s.pose_action,     to_path("/user/hand/left/input/grip/pose")},
            {s.pose_action,     to_path("/user/hand/right/input/grip/pose")},
            {s.grip_action,     to_path(grip_paths[p][0])},
            {s.grip_action,     to_path(grip_paths[p][1])},
            {s.recenter_action, to_path("/user/hand/right/input/thumbstick/click")},
            {s.aim_action,      to_path("/user/hand/left/input/aim/pose")},
            {s.aim_action,      to_path("/user/hand/right/input/aim/pose")},
            {s.select_action,   to_path("/user/hand/left/input/trigger/value")},
            {s.select_action,   to_path("/user/hand/right/input/trigger/value")},
            {s.hand_action,     to_path("/user/hand/right/input/a/click")},
            {s.panel_toggle_action, to_path("/user/hand/left/input/thumbstick/click")},
            {s.scroll_action,   to_path("/user/hand/left/input/thumbstick")},
            {s.turn_action,     to_path("/user/hand/right/input/thumbstick")},
            {s.haptic_action,   to_path("/user/hand/left/output/haptic")},
            {s.haptic_action,   to_path("/user/hand/right/output/haptic")},
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
    s.scroll_action = s.turn_action = s.haptic_action = s.pause_action = XR_NULL_HANDLE;
    s.scroll_x = s.scroll_y = s.turn_x = 0.0f;
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
    if (s.ui_vbo) glDeleteBuffers(1, &s.ui_vbo);
    if (s.ui_vao) glDeleteVertexArrays(1, &s.ui_vao);
    s.ui_vbo = s.ui_vao = 0;
    s.sprite_sizes.clear();
    if (s.sprite_prog) glDeleteProgram(s.sprite_prog);
    s.sprite_vbo = s.sprite_vao = s.sprite_prog = 0;
    for (State::CapturedRect& b : s.info_btn)
    {
        if (b.fbo) glDeleteFramebuffers(1, &b.fbo);
        if (b.tex) glDeleteTextures(1, &b.tex);
        b = State::CapturedRect();
    }
    s.info_btn_count = 0;
    if (s.info_fbo) glDeleteFramebuffers(1, &s.info_fbo);
    if (s.info_tex) glDeleteTextures(1, &s.info_tex);
    s.info_fbo = s.info_tex = 0;
    s.info_w = s.info_h = 0;
    s.info_ready = false;
    if (s.menu_fbo) glDeleteFramebuffers(1, &s.menu_fbo);
    if (s.menu_tex) glDeleteTextures(1, &s.menu_tex);
    s.menu_fbo = s.menu_tex = 0;
    s.menu_w = s.menu_h = 0;
    s.menu_ready = s.menu_placed = false;
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
/** Point on the wrist panel: 0..1 coordinates (v up), and the distance in
 *  front of it (negative once pushed through). False outside its edges. */
bool panel_coords(Vec3 p, float* u, float* v, float* depth)
{
    const Vec3 d = p - s.panel_center;
    *u = (d.x * s.panel_right.x + d.y * s.panel_right.y + d.z * s.panel_right.z) / s.panel_world_w + 0.5f;
    *v = (d.x * s.panel_up.x + d.y * s.panel_up.y + d.z * s.panel_up.z) / s.panel_world_h + 0.5f;
    *depth = d.x * s.panel_normal.x + d.y * s.panel_normal.y + d.z * s.panel_normal.z;
    return *u >= 0.0f && *u <= 1.0f && *v >= 0.0f && *v <= 1.0f;
}

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

/** Copies the whole finished desktop frame into menu_tex. */
void capture_menu(GLuint screen_fbo, int screen_w, int screen_h)
{
    s.menu_ready = false;
    if (!s_menu_wanted || screen_fbo == 0 || screen_w <= 0 || screen_h <= 0)
        return;
    if (s.menu_tex == 0 || screen_w != s.menu_w || screen_h != s.menu_h)
    {
        if (s.menu_tex == 0) glGenTextures(1, &s.menu_tex);
        if (s.menu_fbo == 0) glGenFramebuffers(1, &s.menu_fbo);
        glBindTexture(GL_TEXTURE_2D, s.menu_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, screen_w, screen_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s.menu_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.menu_tex, 0);
        s.menu_w = screen_w;
        s.menu_h = screen_h;
    }
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, screen_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.menu_fbo);
    glBlitFramebuffer(0, 0, screen_w, screen_h, 0, 0, screen_w, screen_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, screen_fbo);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    s.menu_ready = true;
}

/** Puts the menu screen just within arm's reach in front of the head, a
 *  little below eye level and tilted back to face it. Stays put until the
 *  menu goes away (or recenter is pressed) so it can be reached. */
void place_menu(Vec3 head, Vec3 gaze)
{
    constexpr float k_width = 0.62f;     // metres
    constexpr float k_distance = 0.42f;  // in front of the head
    Vec3 fwd = { gaze.x, 0.0f, gaze.z };
    const float flat = Length(fwd);
    fwd = (flat > 0.1f) ? fwd * (1.0f / flat) : Vec3{0.0f, 0.0f, -1.0f};
    s.menu_world_w = k_width;
    s.menu_world_h = k_width * (float)s.menu_h / (float)(s.menu_w > 0 ? s.menu_w : 1);
    s.menu_center = head + fwd * k_distance + Vec3{0.0f, -0.12f, 0.0f};
    // Face the head, as the wrist panel does.
    Vec3 n = head - s.menu_center;
    n = n * (1.0f / Length(n));
    Vec3 right = { n.z, 0.0f, -n.x };
    right = right * (1.0f / std::fmax(Length(right), 1e-4f));
    s.menu_right = right;
    s.menu_up = { n.y * right.z - n.z * right.y, n.z * right.x - n.x * right.z, n.x * right.y - n.y * right.x };
    s.menu_normal = n;
    s.menu_placed = true;
}

Mat4 menu_model()
{
    Mat4 m = Mat4::Identity();
    const Vec3 r = s.menu_right * s.menu_world_w, u = s.menu_up * s.menu_world_h;
    m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
    m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
    m.m[8] = s.menu_normal.x; m.m[9] = s.menu_normal.y; m.m[10] = s.menu_normal.z;
    m.m[12] = s.menu_center.x; m.m[13] = s.menu_center.y; m.m[14] = s.menu_center.z;
    return m;
}

/** Point on the menu screen: 0..1 coordinates (v up), and the distance in
 *  front of it (negative once pushed through). False outside its edges. */
bool menu_coords(Vec3 p, float* u, float* v, float* depth)
{
    const Vec3 d = p - s.menu_center;
    *u = (d.x * s.menu_right.x + d.y * s.menu_right.y + d.z * s.menu_right.z) / s.menu_world_w + 0.5f;
    *v = (d.x * s.menu_up.x + d.y * s.menu_up.y + d.z * s.menu_up.z) / s.menu_world_h + 0.5f;
    *depth = d.x * s.menu_normal.x + d.y * s.menu_normal.y + d.z * s.menu_normal.z;
    return *u >= 0.0f && *u <= 1.0f && *v >= 0.0f && *v <= 1.0f;
}

/** Tip of the Keeper's index finger, following draw_keeper_hand(). */
Vec3 index_fingertip(const Hand& hand)
{
    const float segment_len[3] = { 0.040f, 0.026f, 0.020f };
    const float joint_bend[3] = { 1.25f, 1.45f, 1.0f };
    constexpr float k_rest_curl = 0.08f;
    Mat4 m = Mat4::FromPose(hand.aim_tracked ? hand.aim_rot : hand.rot, hand.pos)
           * Mat4::Translation({0.034f, 0.0f, 0.004f}) * Mat4::Translation({-0.002f, 0.027f, -0.042f});
    for (int j = 0; j < 3; j++)
        m = m * Mat4::RotationY(joint_bend[j] * hand.trigger * (1.0f - s.point_pose) + k_rest_curl) * Mat4::Translation({0.0f, 0.0f, -segment_len[j] * 0.95f});
    return { m.m[12], m.m[13], m.m[14] };
}

/** Touch handling for the menu screen. The index fingertip pressing into the
 *  screen clicks; hovering just in front moves the cursor. The forward ray
 *  with the trigger works too, for out-of-reach spots. */
MenuInput update_menu_touch_raw(const Hand& hand)
{
    MenuInput in;
    s.menu_pointer_valid = false;
    s.menu_by_ray = false;
    if (!s.menu_shown || !hand.tracked)
    {
        s.menu_touching = false;
        return in;
    }
    constexpr float k_press_depth = 0.004f;    // pushed this far in front... or through: press
    constexpr float k_release_depth = 0.015f;  // pulled back past this: release
    constexpr float k_hover_depth = 0.12f;     // within this: cursor follows the finger
    const Vec3 tip = index_fingertip(hand);
    float u, v, depth;
    if (menu_coords(tip, &u, &v, &depth) && depth < k_hover_depth && depth > -0.08f)
    {
        if (s.menu_touching ? (depth > k_release_depth) : (depth < k_press_depth))
            s.menu_touching = !s.menu_touching;
        in.valid = true;
        in.u = u;
        in.v = v;
        in.press = s.menu_touching;
        // Marker on the screen surface under the fingertip.
        s.menu_pointer = s.menu_center + s.menu_right * ((u - 0.5f) * s.menu_world_w) + s.menu_up * ((v - 0.5f) * s.menu_world_h);
        s.menu_pointer_valid = true;
        return in;
    }
    s.menu_touching = false;

    // Ray: forward along the controller, clicking with the trigger.
    if (hand.aim_tracked)
    {
        const Vec3 dir = Rotate(hand.aim_rot, {0.0f, 0.0f, -1.0f});
        const float denom = dir.x * s.menu_normal.x + dir.y * s.menu_normal.y + dir.z * s.menu_normal.z;
        if (std::fabs(denom) > 1e-5f)
        {
            const Vec3 to_center = s.menu_center - hand.aim_pos;
            const float t = (to_center.x * s.menu_normal.x + to_center.y * s.menu_normal.y + to_center.z * s.menu_normal.z) / denom;
            if (t > 0.0f && menu_coords(hand.aim_pos + dir * t, &u, &v, &depth))
            {
                in.valid = true;
                in.u = u;
                in.v = v;
                in.press = hand.trigger > 0.6f;
                s.menu_pointer = hand.aim_pos + dir * t;
                s.menu_pointer_valid = true;
                s.menu_by_ray = true;
            }
        }
    }
    return in;
}

void buzz(int hand, float amplitude, float seconds);

/** Menu touch, plus the confirmation when a press begins: a flash on the
 *  screen and a short buzz. */
MenuInput update_menu_touch(const Hand& hand)
{
    const MenuInput in = update_menu_touch_raw(hand);
    if (s.menu_flash_time > 0.0f)
        s.menu_flash_time -= s.frame_dt;
    if (in.valid && in.press && !s.menu_was_pressed)
    {
        s.menu_flash_time = 0.35f;
        buzz(1, 0.6f, 0.04f);
    }
    s.menu_was_pressed = in.valid && in.press;
    return in;
}

/** A short buzz in one controller (0 left, 1 right), to confirm a press. */
void buzz(int hand, float amplitude, float seconds)
{
    if (s.haptic_action == XR_NULL_HANDLE || s.session == XR_NULL_HANDLE)
        return;
    XrHapticActionInfo info = {XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = s.haptic_action;
    info.subactionPath = s.hand_paths[hand];
    XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.duration = (XrDuration)(seconds * 1e9f);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    xrApplyHapticFeedback(s.session, &info, (const XrHapticBaseHeader*)&vibration);
}

/******************************************************************************/
// Message box: a copy of the objective / info text box the desktop shows over
// the dungeon view, beside the wrist panel; touched like the panel. A button
// above the panel hides or shows it.

/** Copies a rectangle of the finished desktop frame (screen fractions, y from
 *  the top) into its own texture, sized to match. */
bool capture_rect(GLuint screen_fbo, int screen_w, int screen_h, float fx0, float fy0, float fx1, float fy1,
                  State::CapturedRect& out)
{
    const int x0 = (int)(fx0 * screen_w), x1 = (int)(fx1 * screen_w);
    const int y0 = (int)((1.0f - fy1) * screen_h), y1 = (int)((1.0f - fy0) * screen_h);
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0)
        return false;
    if (out.tex == 0 || w != out.w || h != out.h)
    {
        if (out.tex == 0) glGenTextures(1, &out.tex);
        if (out.fbo == 0) glGenFramebuffers(1, &out.fbo);
        glBindTexture(GL_TEXTURE_2D, out.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out.tex, 0);
        out.w = w;
        out.h = h;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, screen_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, out.fbo);
    glBlitFramebuffer(x0, y0, x1, y1, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, screen_fbo);
    return true;
}

/** Copies the message box out of the finished desktop frame into info_tex. */
void capture_info(GLuint screen_fbo, int screen_w, int screen_h)
{
    s.info_ready = false;
    s.info_btn_count = 0;
    if (!s_info_box.visible || screen_fbo == 0 || screen_w <= 0 || screen_h <= 0)
        return;
    {
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        for (int i = 0; i < s_info_box.button_count && i < 6; i++)
        {
            const InfoBoxInfo::Button& b = s_info_box.buttons[i];
            if (!capture_rect(screen_fbo, screen_w, screen_h, b.x0, b.y0, b.x1, b.y1, s.info_btn[s.info_btn_count]))
                break;  // keep the copies in step with s_info_box.buttons
            s.info_btn_count++;
        }
        if (scissor)
            glEnable(GL_SCISSOR_TEST);
    }
    const int x0 = (int)(s_info_box.x0 * screen_w), x1 = (int)(s_info_box.x1 * screen_w);
    // GL rows run bottom up, the box's top down.
    const int y0 = (int)((1.0f - s_info_box.y1) * screen_h), y1 = (int)((1.0f - s_info_box.y0) * screen_h);
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0)
        return;
    if (s.info_tex == 0 || w != s.info_w || h != s.info_h)
    {
        if (s.info_tex == 0) glGenTextures(1, &s.info_tex);
        if (s.info_fbo == 0) glGenFramebuffers(1, &s.info_fbo);
        glBindTexture(GL_TEXTURE_2D, s.info_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s.info_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.info_tex, 0);
        s.info_w = w;
        s.info_h = h;
    }
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, screen_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.info_fbo);
    glBlitFramebuffer(x0, y0, x1, y1, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, screen_fbo);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    s.info_ready = true;
}

/** Point on the message box: 0..1 coordinates (v up) and the distance in front. */
bool info_coords(Vec3 p, float* u, float* v, float* depth)
{
    const Vec3 d = p - s.info_center;
    *u = (d.x * s.info_right.x + d.y * s.info_right.y + d.z * s.info_right.z) / s.info_world_w + 0.5f;
    *v = (d.x * s.info_up.x + d.y * s.info_up.y + d.z * s.info_up.z) / s.info_world_h + 0.5f;
    *depth = d.x * s.info_normal.x + d.y * s.info_normal.y + d.z * s.info_normal.z;
    return *u >= 0.0f && *u <= 1.0f && *v >= 0.0f && *v <= 1.0f;
}

Mat4 info_model()
{
    Mat4 m = Mat4::Identity();
    const Vec3 r = s.info_right * s.info_world_w, u = s.info_up * s.info_world_h;
    m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
    m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
    m.m[8] = s.info_normal.x; m.m[9] = s.info_normal.y; m.m[10] = s.info_normal.z;
    m.m[12] = s.info_center.x; m.m[13] = s.info_center.y; m.m[14] = s.info_center.z;
    return m;
}

constexpr float k_info_button = 0.028f;  // hide / show button size, metres

void draw_press_outline(const Mat4& view_proj, Vec3 at, Vec3 right, Vec3 up, Vec3 normal, bool flashing, float hover_size);

/** Places the message box and the hide button, and handles touches on both.
 *  Returns the touch to pass on to the game (the box's own buttons). */
MenuInput update_info_box(Vec3 head, const Hand& hand)
{
    MenuInput in;
    const bool available = s.in_level && !s.possessing && !s.menu_shown && s.info_ready && s.hands[0].tracked;
    s.info_button_shown = available && s.panel_shown;
    s.info_shown = available && !s.info_hidden;
    if (s.info_button_flash > 0.0f)
        s.info_button_flash -= s.frame_dt;

    // Button: just above the panel's top-right corner.
    if (s.info_button_shown)
    {
        s.info_button_center = s.panel_center + s.panel_up * (s.panel_world_h * 0.5f + k_info_button * 0.7f)
                             + s.panel_right * (s.panel_world_w * 0.5f - k_info_button * 0.5f);
        s.info_button_hover = false;
        if (hand.tracked)
        {
            const Vec3 d = index_fingertip(hand) - s.info_button_center;
            const float du = d.x * s.panel_right.x + d.y * s.panel_right.y + d.z * s.panel_right.z;
            const float dv = d.x * s.panel_up.x + d.y * s.panel_up.y + d.z * s.panel_up.z;
            const float depth = d.x * s.panel_normal.x + d.y * s.panel_normal.y + d.z * s.panel_normal.z;
            const bool over = std::fabs(du) < k_info_button * 0.6f && std::fabs(dv) < k_info_button * 0.6f && depth > -0.05f;
            s.info_button_hover = over && depth < 0.06f;
            if (over && depth < 0.012f && s.info_button_armed)
            {
                s.info_hidden = !s.info_hidden;
                s.info_shown = available && !s.info_hidden;
                s.info_button_armed = false;
                s.info_button_flash = 0.35f;
                buzz(1, 0.6f, 0.04f);
            }
            else if (!over || depth > 0.03f)
            {
                s.info_button_armed = true;
            }
        }
    }
    if (!s.info_shown)
    {
        s.info_finger = false;
        s.info_touching = false;
        return in;
    }

    // Same scale as the wrist panel (both are copies of the same screen), to
    // its right with the tops lined up; above the left hand without it.
    const float metres_per_px = (s.panel_h > 0) ? s.panel_world_h / (float)s.panel_h : 0.30f / 1440.0f;
    s.info_world_w = (float)s.info_w * metres_per_px;
    s.info_world_h = (float)s.info_h * metres_per_px;
    if (s.panel_shown)
    {
        s.info_right = s.panel_right;
        s.info_up = s.panel_up;
        s.info_normal = s.panel_normal;
        s.info_center = s.panel_center + s.panel_right * (s.panel_world_w * 0.5f + 0.02f + s.info_world_w * 0.5f)
                      + s.panel_up * (s.panel_world_h * 0.5f - s.info_world_h * 0.5f);
    }
    else
    {
        s.info_center = s.hands[0].pos + Vec3{0.0f, 0.06f + s.info_world_h * 0.5f, 0.0f};
        Vec3 n = head - s.info_center;
        n = n * (1.0f / std::fmax(Length(n), 1e-4f));
        Vec3 right = { n.z, 0.0f, -n.x };
        right = right * (1.0f / std::fmax(Length(right), 1e-4f));
        s.info_right = right;
        s.info_up = { n.y * right.z - n.z * right.y, n.z * right.x - n.x * right.z, n.x * right.y - n.y * right.x };
        s.info_normal = n;
    }

    // Its own buttons, twice as big, in a centred row just under it.
    s.info_btn_hover = -1;
    {
        constexpr float k_btn_scale = 2.0f;
        constexpr float k_btn_gap = 0.025f;
        float row_w = 0.0f, row_h = 0.0f;
        for (int i = 0; i < s.info_btn_count; i++)
        {
            s.info_btn_world_w[i] = (float)s.info_btn[i].w * metres_per_px * k_btn_scale;
            s.info_btn_world_h[i] = (float)s.info_btn[i].h * metres_per_px * k_btn_scale;
            row_w += s.info_btn_world_w[i] + (i > 0 ? k_btn_gap : 0.0f);
            row_h = std::fmax(row_h, s.info_btn_world_h[i]);
        }
        float x = -row_w * 0.5f;
        const Vec3 row_center = s.info_center - s.info_up * (s.info_world_h * 0.5f + 0.01f + row_h * 0.5f);
        for (int i = 0; i < s.info_btn_count; i++)
        {
            s.info_btn_center[i] = row_center + s.info_right * (x + s.info_btn_world_w[i] * 0.5f);
            x += s.info_btn_world_w[i] + k_btn_gap;
        }
        // The button nearest the fingertip (or where the ray meets the row),
        // with a generous margin around each so they are easy to hit.
        auto nearest_button = [&](Vec3 at, float* out_depth) {
            int best = -1;
            float best_score = 1e9f;
            for (int i = 0; i < s.info_btn_count; i++)
            {
                const Vec3 d = at - s.info_btn_center[i];
                const float du = d.x * s.info_right.x + d.y * s.info_right.y + d.z * s.info_right.z;
                const float dv = d.x * s.info_up.x + d.y * s.info_up.y + d.z * s.info_up.z;
                const float depth = d.x * s.info_normal.x + d.y * s.info_normal.y + d.z * s.info_normal.z;
                const float hw = s.info_btn_world_w[i] * 0.5f + 0.012f, hh = s.info_btn_world_h[i] * 0.5f + 0.012f;
                if (std::fabs(du) > hw || std::fabs(dv) > hh)
                    continue;
                const float score = std::fmax(std::fabs(du) / hw, std::fabs(dv) / hh);
                if (score < best_score)
                {
                    best_score = score;
                    best = i;
                    *out_depth = depth;
                }
            }
            return best;
        };
        auto press_button = [&](int i, bool press) {
            const InfoBoxInfo::Button& b = s_info_box.buttons[i];
            const float bw = s_info_box.x1 - s_info_box.x0, bh = s_info_box.y1 - s_info_box.y0;
            in.valid = true;
            in.u = ((b.x0 + b.x1) * 0.5f - s_info_box.x0) / bw;  // the real button's middle
            in.v = 1.0f - ((b.y0 + b.y1) * 0.5f - s_info_box.y0) / bh;
            in.press = press;
            s.info_btn_hover = i;
        };
        if (hand.tracked && s.info_btn_count > 0)
        {
            float depth = 0.0f;
            const int i = nearest_button(index_fingertip(hand), &depth);
            if (i >= 0 && depth < 0.08f && depth > -0.06f)
            {
                // Pushing in presses it.
                if (s.info_touching ? (depth > 0.015f) : (depth < 0.004f))
                    s.info_touching = !s.info_touching;
                press_button(i, s.info_touching);
                s.info_finger = true;
            }
            else if (hand.aim_tracked)
            {
                // The forward ray on a button, with the trigger.
                s.info_touching = false;
                const Vec3 dir = Rotate(hand.aim_rot, {0.0f, 0.0f, -1.0f});
                const float denom = dir.x * s.info_normal.x + dir.y * s.info_normal.y + dir.z * s.info_normal.z;
                if (std::fabs(denom) > 1e-5f)
                {
                    const Vec3 to_center = s.info_center - hand.aim_pos;
                    const float t = (to_center.x * s.info_normal.x + to_center.y * s.info_normal.y + to_center.z * s.info_normal.z) / denom;
                    float ray_depth = 0.0f;
                    const int j = (t > 0.0f && t < 1.5f) ? nearest_button(hand.aim_pos + dir * t, &ray_depth) : -1;
                    if (j >= 0)
                        press_button(j, hand.trigger > 0.6f);
                }
            }
        }
    }

    // Fingertip on it, as on the panel; else the forward ray with the trigger.
    if (s.info_btn_hover < 0)
        s.info_finger = false;
    if (hand.tracked && s.info_btn_hover < 0)
    {
        float u, v, depth;
        if (info_coords(index_fingertip(hand), &u, &v, &depth) && depth < 0.08f && depth > -0.06f)
        {
            if (s.info_touching ? (depth > 0.015f) : (depth < 0.004f))
                s.info_touching = !s.info_touching;
            in.valid = true;
            in.u = u;
            in.v = v;
            in.press = s.info_touching;
            s.info_finger = true;
        }
        else
        {
            s.info_touching = false;
            if (hand.aim_tracked)
            {
                const Vec3 dir = Rotate(hand.aim_rot, {0.0f, 0.0f, -1.0f});
                const float denom = dir.x * s.info_normal.x + dir.y * s.info_normal.y + dir.z * s.info_normal.z;
                if (std::fabs(denom) > 1e-5f)
                {
                    const Vec3 to_center = s.info_center - hand.aim_pos;
                    const float t = (to_center.x * s.info_normal.x + to_center.y * s.info_normal.y + to_center.z * s.info_normal.z) / denom;
                    if (t > 0.0f && info_coords(hand.aim_pos + dir * t, &u, &v, &depth))
                    {
                        in.valid = true;
                        in.u = u;
                        in.v = v;
                        in.press = hand.trigger > 0.6f;
                    }
                }
            }
        }
        if (in.valid)
            s.info_pointer = s.info_center + s.info_right * ((in.u - 0.5f) * s.info_world_w)
                           + s.info_up * ((in.v - 0.5f) * s.info_world_h);
    }
    if (s.info_flash_time > 0.0f)
        s.info_flash_time -= s.frame_dt;
    if (in.valid && in.press && !s.info_was_pressed)
    {
        s.info_flash_time = 0.35f;
        s.info_btn_flash = s.info_btn_hover;
        buzz(1, 0.6f, 0.04f);
    }
    s.info_was_pressed = in.valid && in.press;
    return in;
}

constexpr float k_cancel_button = 0.04f;  // metres

/** The "back to the hand" button: a red X at the panel's top-left corner, or
 *  beside the left hand while the panel is folded away. Shown whenever a tool
 *  (room, spell, trap, door, sell...) is chosen; tapping it drops the tool. */
void update_cancel_button(Vec3 head, const Hand& hand)
{
    s.cancel_shown = s.in_level && !s.possessing && !s.menu_shown && !s_plain_hand && s.hands[0].tracked;
    if (s.cancel_flash > 0.0f)
        s.cancel_flash -= s.frame_dt;
    if (!s.cancel_shown)
    {
        s.cancel_hover = false;
        s.cancel_armed = true;
        return;
    }
    if (s.panel_shown)
    {
        s.cancel_right = s.panel_right;
        s.cancel_up = s.panel_up;
        s.cancel_normal = s.panel_normal;
        s.cancel_center = s.panel_center + s.panel_up * (s.panel_world_h * 0.5f + k_cancel_button * 0.7f)
                        - s.panel_right * (s.panel_world_w * 0.5f - k_cancel_button * 0.5f);
    }
    else
    {
        const Vec3 base = s.hands[0].pos + Vec3{0.0f, 0.07f, 0.0f};
        Vec3 n = head - base;
        n = n * (1.0f / std::fmax(Length(n), 1e-4f));
        Vec3 right = { n.z, 0.0f, -n.x };
        right = right * (1.0f / std::fmax(Length(right), 1e-4f));
        s.cancel_right = right;
        s.cancel_up = { n.y * right.z - n.z * right.y, n.z * right.x - n.x * right.z, n.x * right.y - n.y * right.x };
        s.cancel_normal = n;
        // Left of the message box, which sits above the hand.
        const float aside = s.info_shown ? s.info_world_w * 0.5f + k_cancel_button : 0.0f;
        s.cancel_center = base - right * aside;
    }
    s.cancel_hover = false;
    if (!hand.tracked)
        return;
    const Vec3 d = index_fingertip(hand) - s.cancel_center;
    const float du = d.x * s.cancel_right.x + d.y * s.cancel_right.y + d.z * s.cancel_right.z;
    const float dv = d.x * s.cancel_up.x + d.y * s.cancel_up.y + d.z * s.cancel_up.z;
    const float depth = d.x * s.cancel_normal.x + d.y * s.cancel_normal.y + d.z * s.cancel_normal.z;
    const bool over = std::fabs(du) < k_cancel_button * 0.75f && std::fabs(dv) < k_cancel_button * 0.75f && depth > -0.05f;
    s.cancel_hover = over && depth < 0.06f;
    if (over && depth < 0.012f && s.cancel_armed)
    {
        s_cancel_request = true;
        s.cancel_armed = false;
        s.cancel_flash = 0.35f;
        buzz(1, 0.6f, 0.04f);
    }
    else if (!over || depth > 0.03f)
    {
        s.cancel_armed = true;
    }
}

/** Draws the "back to the hand" button. Solid program and cube bound. */
void draw_cancel_button(const Mat4& view_proj)
{
    if (!s.cancel_shown)
        return;
    const float grow = (s.cancel_flash > 0.0f) ? 1.25f : s.cancel_hover ? 1.12f : 1.0f;
    const float size = k_cancel_button * grow;
    auto plate = [&](Vec3 r, Vec3 u, float lift) {
        Mat4 m = Mat4::Identity();
        const Vec3 dz = s.cancel_normal * 0.003f;
        const Vec3 at = s.cancel_center + s.cancel_normal * lift;
        m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
        m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
        m.m[8] = dz.x; m.m[9] = dz.y; m.m[10] = dz.z;
        m.m[12] = at.x; m.m[13] = at.y; m.m[14] = at.z;
        const Mat4 mvp = view_proj * m;
        glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, mvp.m);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    };
    if (s.cancel_flash > 0.0f)
        glUniform4f(s.loc_solid_color, 0.45f, 1.0f, 0.45f, 1.0f);
    else
        glUniform4f(s.loc_solid_color, 0.80f, 0.12f, 0.10f, 1.0f);
    plate(s.cancel_right * size, s.cancel_up * size, 0.0f);
    // A white X: two bars along the diagonals.
    const float k = 0.70710678f;
    const Vec3 d1 = (s.cancel_right + s.cancel_up) * k, d2 = (s.cancel_right - s.cancel_up) * k;
    glUniform4f(s.loc_solid_color, 0.95f, 0.92f, 0.85f, 1.0f);
    plate(d1 * (size * 0.8f), d2 * (size * 0.16f), 0.003f);
    plate(d2 * (size * 0.8f), d1 * (size * 0.16f), 0.003f);
}

/** New text: raise the alert when a message box opens or shows a different
 *  message, buzz the left controller every couple of seconds (a few times),
 *  and drop it once the text is looked at. */
void update_text_alert(Vec3 gaze)
{
    const bool visible = s_info_box.visible && s.in_level;
    if (visible && (!s.text_was_visible || s_info_box.message_id != s.text_last_message))
    {
        s.text_alert = true;
        s.text_alert_pulses = 0;
        s.text_alert_timer = 0.0f;
    }
    s.text_was_visible = visible;
    if (visible)
        s.text_last_message = s_info_box.message_id;
    if (!visible)
        s.text_alert = false;
    if (!s.text_alert)
        return;

    // Looked at: the box is shown in VR and the head points at it (within about 20 degrees).
    if (s.info_shown)
    {
        Vec3 to_box = s.info_center - s.last_head;
        const float len = Length(to_box);
        if (len > 0.01f)
        {
            to_box = to_box * (1.0f / len);
            if (to_box.x * gaze.x + to_box.y * gaze.y + to_box.z * gaze.z > 0.94f)
            {
                s.text_alert = false;
                return;
            }
        }
    }
    constexpr int k_pulses = 4;
    constexpr float k_pulse_every = 1.8f;  // seconds
    s.text_alert_timer -= s.frame_dt;
    if (s.text_alert_timer <= 0.0f && s.text_alert_pulses < k_pulses)
    {
        buzz(0, 0.45f, 0.08f);
        s.text_alert_pulses++;
        s.text_alert_timer = k_pulse_every;
    }
}

/** Draws the message box and the hide button. Solid program and cube bound. */
void draw_info_box(const Mat4& view_proj)
{
    if (s.info_button_shown)
    {
        // A small tablet: gold while the text shows, dim while hidden, with
        // two dark "text lines" on it.
        const float grow = (s.info_button_flash > 0.0f) ? 1.25f : s.info_button_hover ? 1.12f : 1.0f;
        const float size = k_info_button * grow;
        auto plate = [&](Vec3 c, float w, float h, float lift) {
            Mat4 m = Mat4::Identity();
            const Vec3 r = s.panel_right * w, u = s.panel_up * h, d = s.panel_normal * 0.003f;
            const Vec3 at = c + s.panel_normal * lift;
            m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
            m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
            m.m[8] = d.x; m.m[9] = d.y; m.m[10] = d.z;
            m.m[12] = at.x; m.m[13] = at.y; m.m[14] = at.z;
            const Mat4 mvp = view_proj * m;
            glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, mvp.m);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        if (s.info_button_flash > 0.0f)
            glUniform4f(s.loc_solid_color, 0.45f, 1.0f, 0.45f, 1.0f);
        else if (s.info_hidden)
            glUniform4f(s.loc_solid_color, 0.40f, 0.36f, 0.30f, 1.0f);
        else
            glUniform4f(s.loc_solid_color, 1.0f, 0.82f, 0.25f, 1.0f);
        plate(s.info_button_center, size, size, 0.0f);
        glUniform4f(s.loc_solid_color, 0.15f, 0.08f, 0.05f, 1.0f);
        plate(s.info_button_center + s.panel_up * (size * 0.15f), size * 0.62f, size * 0.1f, 0.003f);
        plate(s.info_button_center - s.panel_up * (size * 0.15f), size * 0.45f, size * 0.1f, 0.003f);
    }
    if (!s.info_shown)
        return;
    glUseProgram(s.panel_prog);
    const Mat4 mvp = view_proj * info_model();
    glUniformMatrix4fv(s.loc_panel_mvp, 1, GL_FALSE, mvp.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s.info_tex);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    // The repeated buttons under it.
    for (int i = 0; i < s.info_btn_count; i++)
    {
        Mat4 m = Mat4::Identity();
        const Vec3 r = s.info_right * s.info_btn_world_w[i], u = s.info_up * s.info_btn_world_h[i];
        const Vec3 c = s.info_btn_center[i];
        m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
        m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
        m.m[8] = s.info_normal.x; m.m[9] = s.info_normal.y; m.m[10] = s.info_normal.z;
        m.m[12] = c.x; m.m[13] = c.y; m.m[14] = c.z;
        const Mat4 bmvp = view_proj * m;
        glUniformMatrix4fv(s.loc_panel_mvp, 1, GL_FALSE, bmvp.m);
        glBindTexture(GL_TEXTURE_2D, s.info_btn[i].tex);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(s.solid_prog);
    if (s.info_btn_hover >= 0)
    {
        const int i = s.info_btn_hover;
        const bool flashing = s.info_flash_time > 0.0f && s.info_btn_flash == i;
        draw_press_outline(view_proj, s.info_btn_center[i], s.info_right, s.info_up, s.info_normal, flashing,
                           std::fmax(s.info_btn_world_w[i], s.info_btn_world_h[i]) * 1.1f);
    }
    else if (s_info_in.valid)
    {
        draw_press_outline(view_proj, s.info_pointer, s.info_right, s.info_up, s.info_normal,
                           s.info_flash_time > 0.0f, 0.018f);
    }
}

/******************************************************************************/
// Palm menu: turn the left palm up and the creature types you own float above
// it; tap one with the right index finger to pick up one of them.

struct UiDraw {
    int32_t sprite_id;
    int first_vertex;
};
std::vector<SpriteVertex> s_ui_verts;
std::vector<UiDraw> s_ui_draws;

/** A highlight frame behind a palm icon: gold while hovered, green just after a tap. */
struct UiFrame {
    Vec3 center;
    float w, h;
    bool tapped;
};
std::vector<UiFrame> s_ui_frames;

constexpr int k_palm_columns = 4;
constexpr float k_palm_icon = 0.045f;    // icon height, metres
constexpr float k_palm_gap = 0.012f;

/** Centre of icon @p i in the grid floating above the palm. */
Vec3 palm_icon_center(Vec3 origin, int i, int rows)
{
    const int row = i / k_palm_columns, col = i % k_palm_columns;
    const float step = k_palm_icon + k_palm_gap;
    const float x = ((float)col - (float)(k_palm_columns - 1) * 0.5f) * step;
    const float y = ((float)(rows - 1) * 0.5f - (float)row) * (step + 0.012f);  // room for the count below
    return origin + s.palm_right * x + s.palm_up * y;
}

void add_ui_quad(int32_t sprite_id, Vec3 center, float w, float h)
{
    const Vec3 r = s.palm_right * (w * 0.5f), u = s.palm_up * (h * 0.5f);
    const Vec3 tl = center - r + u, tr = center + r + u, br = center + r - u, bl = center - r - u;
    s_ui_draws.push_back({ sprite_id, (int)s_ui_verts.size() });
    const SpriteVertex v[6] = {
        { tl.x, tl.y, tl.z, 0, 0, -1, -1 }, { tr.x, tr.y, tr.z, 1, 0, -1, -1 }, { br.x, br.y, br.z, 1, 1, -1, -1 },
        { br.x, br.y, br.z, 1, 1, -1, -1 }, { bl.x, bl.y, bl.z, 0, 1, -1, -1 }, { tl.x, tl.y, tl.z, 0, 0, -1, -1 },
    };
    s_ui_verts.insert(s_ui_verts.end(), v, v + 6);
}

/** Lays out the palm menu, handles taps, and builds its quads for this frame. */
void update_palm_menu(Vec3 head)
{
    s_ui_verts.clear();
    s_ui_draws.clear();
    s_ui_frames.clear();
    if (s.palm_flash_time > 0.0f)
        s.palm_flash_time -= s.frame_dt;
    const Hand& left = s.hands[0];
    const Vec3 palm_normal = Rotate(left.aim_rot, {1.0f, 0.0f, 0.0f});  // a left palm faces +X
    s.palm_shown = s.in_level && !s.possessing && !s.menu_shown && !s.ray_left && left.tracked && left.aim_tracked
                && palm_normal.y > 0.6f && !s_things.palm.empty();
    if (!s.palm_shown)
    {
        s.palm_touched = -1;
        s.palm_hover = -1;
        s.palm_armed = true;
        return;
    }
    // A grid standing just above the palm, turned to face the head.
    const Vec3 origin = left.pos + Vec3{0.0f, 0.13f, 0.0f};
    Vec3 n = head - origin;
    n = n * (1.0f / std::fmax(Length(n), 1e-4f));
    Vec3 right = { n.z, 0.0f, -n.x };
    right = right * (1.0f / std::fmax(Length(right), 1e-4f));
    s.palm_right = right;
    s.palm_up = { n.y * right.z - n.z * right.y, n.z * right.x - n.x * right.z, n.x * right.y - n.y * right.x };
    s.palm_normal = n;

    const int count = (int)s_things.palm.size();
    const int rows = (count + k_palm_columns - 1) / k_palm_columns;
    // Which icon the right index fingertip is in front of (hover) or pushed into (touch).
    s.palm_touched = -1;
    s.palm_hover = -1;
    if (s.hands[1].tracked)
    {
        const Vec3 tip = index_fingertip(s.hands[1]);
        for (int i = 0; i < count; i++)
        {
            const Vec3 d = tip - palm_icon_center(origin, i, rows);
            const float du = d.x * s.palm_right.x + d.y * s.palm_right.y + d.z * s.palm_right.z;
            const float dv = d.x * s.palm_up.x + d.y * s.palm_up.y + d.z * s.palm_up.z;
            const float depth = d.x * n.x + d.y * n.y + d.z * n.z;
            if (std::fabs(du) < k_palm_icon * 0.55f && std::fabs(dv) < k_palm_icon * 0.55f && depth > -0.05f)
            {
                if (depth < 0.06f)
                    s.palm_hover = i;
                if (depth < 0.015f)
                    s.palm_touched = i;
                if (s.palm_hover >= 0)
                    break;
            }
        }
    }
    if (s.palm_touched >= 0 && s.palm_armed)
    {
        s_palm_pick_kind = s_things.palm[(size_t)s.palm_touched].kind;
        s.palm_armed = false;  // one creature per tap
        s.palm_flash = s.palm_touched;
        s.palm_flash_time = 0.35f;
        buzz(1, 0.6f, 0.04f);
    }
    else if (s.palm_touched < 0)
    {
        s.palm_armed = true;
    }

    for (int i = 0; i < count; i++)
    {
        const PalmItem& item = s_things.palm[(size_t)i];
        const Vec3 c = palm_icon_center(origin, i, rows);
        const auto size = s.sprite_sizes.find(item.sprite_id);
        float aspect = 1.0f;
        if (size != s.sprite_sizes.end() && size->second.second > 0)
            aspect = (float)size->second.first / (float)size->second.second;
        const bool flashing = (i == s.palm_flash && s.palm_flash_time > 0.0f);
        const bool hovered = (i == s.palm_hover);
        const float grow = flashing ? 1.3f : hovered ? 1.15f : 1.0f;
        add_ui_quad(item.sprite_id, c, k_palm_icon * aspect * grow, k_palm_icon * grow);
        if (flashing || hovered)
            s_ui_frames.push_back({ c, k_palm_icon * aspect * grow + 0.008f, k_palm_icon * grow + 0.008f, flashing });
        // The count, small and centred under the icon.
        char digits[8];
        const int len = std::snprintf(digits, sizeof(digits), "%d", item.count);
        const float dh = 0.012f, dw = dh * s_things.digit_aspect;
        const Vec3 below = c - s.palm_up * (k_palm_icon * 0.5f + dh * 0.6f);
        for (int k = 0; k < len; k++)
        {
            const int d = digits[k] - '0';
            if (d < 0 || d > 9 || s_things.digit_sprite[d] == 0)
                continue;
            add_ui_quad(s_things.digit_sprite[d], below + s.palm_right * (((float)k - (float)(len - 1) * 0.5f) * dw), dw, dh);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, s.ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_ui_verts.size() * sizeof(SpriteVertex)),
                 s_ui_verts.empty() ? nullptr : s_ui_verts.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/** Draws the palm menu. Expects the palette and fade table bound on units 1
 *  and 3 (the lightmap is not used: the icons are fully lit). */
void draw_palm_menu(const Mat4& view_proj, const GLWorldViewRenderer::VRTextures& tex)
{
    if (!s.palm_shown || s_ui_draws.empty())
        return;

    // Highlight frames, a little behind their icons.
    glUseProgram(s.solid_prog);
    glBindVertexArray(s.cube_vao);
    for (const UiFrame& f : s_ui_frames)
    {
        Mat4 m = Mat4::Identity();
        const Vec3 r = s.palm_right * f.w, u = s.palm_up * f.h, d = s.palm_normal * 0.002f;
        const Vec3 c = f.center - s.palm_normal * 0.003f;
        m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
        m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
        m.m[8] = d.x; m.m[9] = d.y; m.m[10] = d.z;
        m.m[12] = c.x; m.m[13] = c.y; m.m[14] = c.z;
        const Mat4 mvp = view_proj * m;
        glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, mvp.m);
        if (f.tapped)
            glUniform4f(s.loc_solid_color, 0.45f, 1.0f, 0.45f, 1.0f);
        else
            glUniform4f(s.loc_solid_color, 1.0f, 0.82f, 0.25f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    glBindVertexArray(0);

    glUseProgram(s.sprite_prog);
    glUniformMatrix4fv(s.loc_sprite_mvp, 1, GL_FALSE, view_proj.m);
    glUniform1i(s.loc_sprite_use_fade, tex.fade != 0 ? 1 : 0);
    glUniform1f(s.loc_sprite_alpha, 1.0f);
    glUniform1i(s.loc_sprite_shadow, 0);
    glUniform1f(s.loc_sprite_one_colour, -1.0f);
    glBindVertexArray(s.ui_vao);
    glActiveTexture(GL_TEXTURE0);
    for (const UiDraw& d : s_ui_draws)
    {
        const auto it = s.sprite_textures.find(d.sprite_id);
        if (it == s.sprite_textures.end())
            continue;
        glBindTexture(GL_TEXTURE_2D, it->second);
        glDrawArrays(GL_TRIANGLES, d.first_vertex, 6);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
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
        s.sprite_sizes[img.id] = { img.w, img.h };
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
/** Where the dungeon is in the reference space: the game point @c focus sits
 *  at @c pos, turned by @c yaw and scaled by @c scale (metres per game unit).
 *  Either the tabletop, or life size around the possessed creature's eyes. */
struct WorldPlacement {
    Vec3 pos;
    float yaw;
    float scale;
    Vec3 focus;   // game units: x, y on the map, z up
};

WorldPlacement world_placement()
{
    if (s.possessing)
        return { s.fp_anchor, s.fp_yaw, k_fp_scale, s.fp_eye };
    return { s.table_pos, s.table_yaw, s.table_scale, {s.table_focus_x, s.table_focus_y, 0.0f} };
}

/** Game units -> reference space. Game x runs right, game y (south)
 *  towards the player, game z up. */
Mat4 table_model()
{
    const WorldPlacement w = world_placement();
    Mat4 swizzle = Mat4::Identity();
    swizzle.m[5] = 0.0f; swizzle.m[6] = 1.0f;   // game y -> +Z
    swizzle.m[9] = 1.0f; swizzle.m[10] = 0.0f;  // game z -> +Y
    return Mat4::Translation(w.pos) * Mat4::RotationY(w.yaw)
         * Mat4::Scale({w.scale, w.scale, w.scale}) * swizzle
         * Mat4::Translation({-w.focus.x, -w.focus.y, -w.focus.z});
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

/** Right thumbstick while possessing: smooth turning. The dungeon turns
 *  around the head (so the head keeps its place in it), and with it the way
 *  the creature faces, which follows the head. */
void update_possession_turn(float dt)
{
    constexpr float k_deadzone = 0.2f;
    constexpr float k_turn_speed = 2.1f;  // radians per second at full deflection (about 120 degrees)
    const float mag = std::fabs(s.turn_x);
    if (mag <= k_deadzone || dt <= 0.0f)
        return;
    const float eased = (mag - k_deadzone) / (1.0f - k_deadzone);
    // Pushing right turns right: the world turns the other way around the head.
    const float delta = (s.turn_x > 0.0f ? 1.0f : -1.0f) * eased * eased * k_turn_speed * dt;
    const Vec3 pivot = s.last_head;
    s.fp_anchor = pivot + RotateY(s.fp_anchor - pivot, delta);
    s.fp_yaw += delta;
}

/** Right thumbstick at the table: turns it smoothly about the spot half a
 *  metre in front of the head, so what is in front stays in reach while the
 *  rest swings round. Same feel as update_possession_turn(). */
void update_table_turn(float dt)
{
    constexpr float k_deadzone = 0.2f;
    constexpr float k_turn_speed = 1.6f;  // radians per second at full deflection
    const float mag = std::fabs(s.turn_x);
    if (s.grab_mask != 0 || mag <= k_deadzone || dt <= 0.0f)
        return;
    const float eased = (mag - k_deadzone) / (1.0f - k_deadzone);
    const float delta = (s.turn_x > 0.0f ? 1.0f : -1.0f) * eased * eased * k_turn_speed * dt;
    const Vec3 pivot = s.last_head + s.head_forward * 0.5f;
    s.table_pos = pivot + RotateY(s.table_pos - pivot, delta);
    s.table_yaw += delta;
}

void update_input(XrTime time, float dt)
{
    s.frame_dt = dt;
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
    {
        if (s.menu_shown)
            s.menu_placed = false;  // bring the menu screen back in front
        else if (s.possessing)
            s.fp_reanchor = true;   // put the creature's eyes back at the head
        else
            s.table_placed = false;
    }

    get.action = s.panel_toggle_action;
    XrActionStateBoolean toggle = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &get, &toggle)) &&
        toggle.isActive && toggle.changedSinceLastSync && toggle.currentState)
    {
        s.panel_hidden = !s.panel_hidden;
        s.panel_auto_hidden = false;  // the player decides from here
    }

    get.action = s.pause_action;
    XrActionStateBoolean pause = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &get, &pause)) &&
        pause.isActive && pause.changedSinceLastSync && pause.currentState)
    {
        s_pause_request = true;
        buzz(0, 0.5f, 0.05f);
    }

    get.action = s.scroll_action;
    XrActionStateVector2f stick = {XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(s.session, &get, &stick)) && stick.isActive)
    {
        s.scroll_x = stick.currentState.x;
        s.scroll_y = stick.currentState.y;
    }
    get.action = s.turn_action;
    XrActionStateVector2f turn = {XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(s.session, &get, &turn)) && turn.isActive)
        s.turn_x = turn.currentState.x;

    // While possessing, the left stick walks the creature (see
    // update_possession()), the right one turns, and the table stays where it
    // was for when the player leaves it.
    if (s.possessing)
    {
        update_possession_turn(dt);
    }
    else
    {
        update_table_grab();
        update_table_scroll(dt);
        update_table_turn(dt);
    }
}

/** Possession: switches between the table and the creature's eyes, and turns
 *  the head's direction into the facing the creature should take. */
void update_possession(Vec3 head, Vec3 gaze)
{
    const bool was_possessing = s.possessing;
    s.possessing = s_possession.active;
    if (!s.possessing || !s_possession.digger || !was_possessing)
        s.swing_have_last = false;  // no stale pickaxe position across possessions
    if (!s.possessing)
    {
        s_look.valid = false;
        return;
    }
    s.fp_eye = { s_possession.eye_x, s_possession.eye_y, s_possession.eye_z };
    const float flat = std::sqrt(gaze.x * gaze.x + gaze.z * gaze.z);
    if (!was_possessing || s.fp_reanchor)
    {
        // Put the creature's eyes at the head, facing the way the head faces.
        s.fp_reanchor = false;
        s.fp_anchor = head;
        const float a = (float)s_possession.yaw * 3.14159265f / 1024.0f;
        const Vec3 creature_fwd = { std::sin(a), 0.0f, -std::cos(a) };  // game (x, y) on the local x/z plane
        const Vec3 head_fwd = (flat > 0.1f) ? Vec3{gaze.x, 0.0f, gaze.z} : Vec3{0.0f, 0.0f, -1.0f};
        s.fp_yaw = Heading(head_fwd) - Heading(creature_fwd);
    }

    // Head direction in game terms: yaw from the horizontal part, pitch from
    // how far up or down it looks (the game allows about +-40 degrees).
    const Vec3 local = RotateY(gaze, -s.fp_yaw);
    PossessionLook look;
    look.valid = flat > 0.1f;
    look.yaw = (int)(LbArcTanAngle((int32_t)(local.x * 10000.0f), (int32_t)(local.z * 10000.0f)) & ANGLE_MASK);
    const float up = std::fmax(-1.0f, std::fmin(1.0f, gaze.y));
    int pitch = (int)std::lround(std::asin(up) * 1024.0f / 3.14159265f);
    pitch = (pitch > 227) ? 227 : (pitch < -227) ? -227 : pitch;
    look.pitch = pitch & ANGLE_MASK;
    look.move_x = (int)(s.scroll_x * 100.0f);
    look.move_y = (int)(s.scroll_y * 100.0f);
    s_look = look;
}

Vec3 stage_to_game(Vec3 p);
void buzz(int hand, float amplitude, float seconds);

/** Slap gesture: the right palm swatting low over the table. The game thread
 *  picks the nearest slappable creature to where it swept (generously, as
 *  creatures move fast) and flashes it while the palm is near. */
void update_slap(const Hand& hand)
{
    SlapInput in;
    if (s_slap_landed)
    {
        s_slap_landed = false;
        buzz(1, 1.0f, 0.09f);  // a solid thwack
    }
    const bool can = s.in_level && !s.possessing && !s.menu_shown && !s.panel_finger && !s.palm_shown
                  && hand.tracked && hand.aim_tracked && hand.grip < 0.5f && !s_select_held;
    if (!can)
    {
        s.slap_have_last = false;
        s_slap_in = in;
        return;
    }
    // Where the hand strikes: about the middle of the fingers, ahead of the
    // palm (the aim pose's -Z runs along them), where a swat lands and moves fastest.
    const Vec3 palm = hand.pos + Rotate(hand.aim_rot, {0.028f, 0.0f, -0.075f});
    const Vec3 g = stage_to_game(palm);
    // Low over the table: within about three cubes of the floor (whatever the table's size).
    in.low = g.z < 256.0f + 3.0f * 256.0f && g.z > -256.0f;
    in.x = g.x;
    in.y = g.y;
    // About 7 cm on the table, but never less than a slab and a half.
    in.radius = std::fmax(384.0f, 0.07f / s.table_scale);
    s.slap_cooldown -= s.frame_dt;
    if (s.slap_have_last && s.frame_dt > 0.0f)
    {
        constexpr float k_swat_speed = 1.6f;  // m/s
        const float speed = Length(palm - s.slap_last) / s.frame_dt;
        if (in.low && speed > k_swat_speed && s.slap_cooldown <= 0.0f)
        {
            in.swipe = true;
            s.slap_cooldown = 0.45f;  // one slap per swat
        }
    }
    s.slap_last = palm;
    s.slap_have_last = true;
    s_slap_in = in;
}

/** Game units -> reference space; same as table_model() applied to a point. */
Vec3 game_to_stage(Vec3 g)
{
    const WorldPlacement w = world_placement();
    const Vec3 local = { (g.x - w.focus.x) * w.scale, (g.z - w.focus.z) * w.scale, (g.y - w.focus.y) * w.scale };
    return RotateY(local, w.yaw) + w.pos;
}

/** Reference space -> game units; inverse of table_model(). */
Vec3 stage_to_game(Vec3 p)
{
    const WorldPlacement w = world_placement();
    const Vec3 q = RotateY(p - w.pos, -w.yaw) * (1.0f / w.scale);
    return { q.x + w.focus.x, q.z + w.focus.y, q.y + w.focus.z };
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

/** The room / dig selection: a see-through tint over each slab, gold where it
 *  can be built or dug, red where it cannot. @p mvp maps game units. */
void draw_selection(const Mat4& mvp)
{
    if (s_things.selection.empty())
        return;
    glUseProgram(s.solid_prog);
    glBindVertexArray(s.cube_vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (const SelectionTile& t : s_things.selection)
    {
        // A thin slab-sized plate just above the surface, inset a little so
        // neighbouring tiles show their edges.
        constexpr float k_inset = 10.0f;
        const Mat4 m = mvp * Mat4::Translation({(t.x0 + t.x1) * 0.5f, (t.y0 + t.y1) * 0.5f, t.z + 6.0f})
                     * Mat4::Scale({t.x1 - t.x0 - 2.0f * k_inset, t.y1 - t.y0 - 2.0f * k_inset, 8.0f});
        glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, m.m);
        if (t.valid)
            glUniform4f(s.loc_solid_color, 0.95f, 0.80f, 0.25f, 0.30f);
        else
            glUniform4f(s.loc_solid_color, 0.90f, 0.15f, 0.10f, 0.35f);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

/** A square outline around a pointer on a flat screen (menu screen, wrist
 *  panel): gold while hovering, green and larger for a moment once a press
 *  lands, as the palm icons do. Uses the solid program and cube, bound. */
void draw_press_outline(const Mat4& view_proj, Vec3 at, Vec3 right, Vec3 up, Vec3 normal, bool flashing, float hover_size)
{
    const float size = flashing ? hover_size * 1.55f : hover_size;
    const float bar = 0.003f;
    const Vec3 c = at + normal * 0.002f;
    if (flashing)
        glUniform4f(s.loc_solid_color, 0.45f, 1.0f, 0.45f, 1.0f);
    else
        glUniform4f(s.loc_solid_color, 1.0f, 0.82f, 0.25f, 1.0f);
    for (int side = 0; side < 4; side++)
    {
        const bool horizontal = (side < 2);
        const float offset = (side % 2 == 0 ? 0.5f : -0.5f) * size;
        const Vec3 centre = horizontal ? c + up * offset : c + right * offset;
        const Vec3 r = right * (horizontal ? size + bar : bar);
        const Vec3 u = up * (horizontal ? bar : size + bar);
        const Vec3 d = normal * 0.001f;
        Mat4 m = Mat4::Identity();
        m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
        m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
        m.m[8] = d.x; m.m[9] = d.y; m.m[10] = d.z;
        m.m[12] = centre.x; m.m[13] = centre.y; m.m[14] = centre.z;
        const Mat4 mvp = view_proj * m;
        glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, mvp.m);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
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
    // Pointing at a button (palm menu, menu screen): index straight, the rest curled.
    const float point = (&hand == &s.hands[1]) ? s.point_pose : 0.0f;
    const float index_curl = hand.trigger * (1.0f - point);
    const float fist_curl = std::fmax(hand.grip, point);

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

constexpr float k_pick_handle_len = 0.42f;

/** The pickaxe's frame: at the fist, -Z up the handle. It is held nearly
 *  upright (tilted about 75 degrees up from the controller's forward), and
 *  the trigger tips it forward as if striking. */
Mat4 pickaxe_frame(const Hand& hand)
{
    return Mat4::FromPose(hand.aim_tracked ? hand.aim_rot : hand.rot, hand.pos)
         * Mat4::RotationX(1.3f - 1.1f * hand.trigger);
}

/** Centre of the pickaxe head, reference space. */
Vec3 pickaxe_head(const Hand& hand)
{
    const Mat4 m = pickaxe_frame(hand);
    const float z = 0.06f - k_pick_handle_len;
    return { m.m[8] * z + m.m[12], m.m[9] * z + m.m[13], m.m[10] * z + m.m[14] };
}

/** A pickaxe in the right hand: the handle rises from the fist, the head
 *  crosses it at the top with the point facing forward and a broader blade
 *  behind. */
void draw_pickaxe(const Mat4& view_proj, const Hand& hand)
{
    constexpr float wood[3]  = { 0.45f, 0.28f, 0.14f };
    constexpr float iron[3]  = { 0.52f, 0.53f, 0.58f };
    constexpr float strap[3] = { 0.28f, 0.17f, 0.09f };

    glUseProgram(s.lit_prog);
    const float light_len = std::sqrt(0.3f * 0.3f + 0.9f * 0.9f + 0.4f * 0.4f);
    glUniform3f(s.loc_lit_light, 0.3f / light_len, 0.9f / light_len, 0.4f / light_len);
    glBindVertexArray(s.lit_cube_vao);

    const Mat4 grip = pickaxe_frame(hand);
    // Handle, starting a little below the fist.
    draw_lit_box(view_proj, grip * Mat4::Translation({0.0f, 0.0f, 0.06f - k_pick_handle_len * 0.5f})
                            * Mat4::Scale({0.026f, 0.026f, k_pick_handle_len}), wood[0], wood[1], wood[2]);
    // Fist-sized wrap where the hand holds it, and one under the head.
    draw_lit_box(view_proj, grip * Mat4::Scale({0.032f, 0.032f, 0.07f}), strap[0], strap[1], strap[2]);
    const Mat4 tip = grip * Mat4::Translation({0.0f, 0.0f, 0.06f - k_pick_handle_len});
    draw_lit_box(view_proj, tip * Mat4::Translation({0.0f, 0.0f, 0.03f}) * Mat4::Scale({0.034f, 0.034f, 0.05f}),
                 strap[0], strap[1], strap[2]);
    // Head: in this frame -Y faces forward once the pickaxe is upright, so the
    // point goes there and the blade on +Y. Both curve down towards the hand.
    draw_lit_box(view_proj, tip * Mat4::Scale({0.036f, 0.07f, 0.05f}), iron[0], iron[1], iron[2]);
    draw_lit_box(view_proj, tip * Mat4::Translation({0.0f, -0.075f, 0.008f}) * Mat4::RotationX(0.25f)
                            * Mat4::Scale({0.024f, 0.09f, 0.032f}), iron[0], iron[1], iron[2]);
    draw_lit_box(view_proj, tip * Mat4::Translation({0.0f, -0.135f, 0.03f}) * Mat4::RotationX(0.5f)
                            * Mat4::Scale({0.014f, 0.05f, 0.018f}), iron[0], iron[1], iron[2]);
    draw_lit_box(view_proj, tip * Mat4::Translation({0.0f, 0.075f, 0.008f}) * Mat4::RotationX(-0.25f)
                            * Mat4::Scale({0.03f, 0.09f, 0.04f}), iron[0], iron[1], iron[2]);
    draw_lit_box(view_proj, tip * Mat4::Translation({0.0f, 0.13f, 0.03f}) * Mat4::RotationX(-0.45f)
                            * Mat4::Scale({0.022f, 0.04f, 0.05f}), iron[0], iron[1], iron[2]);
    glBindVertexArray(0);
}

/** Swinging the pickaxe: true for a few frames after its head comes down fast
 *  enough, which then counts as a click (dig / attack) like the trigger. */
bool update_pickaxe_swing(const Hand& hand, float dt)
{
    constexpr float k_strike_speed = 2.2f;   // m/s downward at the head
    constexpr float k_cooldown = 0.35f;      // s between strikes, so one swing is one strike
    constexpr int k_click_frames = 4;        // long enough for the game to see the press
    const Vec3 head = pickaxe_head(hand);
    if (s.swing_have_last && dt > 0.0f)
    {
        const Vec3 v = (head - s.swing_last_head) * (1.0f / dt);
        s.swing_cooldown -= dt;
        if (s.swing_cooldown <= 0.0f && -v.y > k_strike_speed)
        {
            s.swing_click_frames = k_click_frames;
            s.swing_cooldown = k_cooldown;
        }
    }
    s.swing_last_head = head;
    s.swing_have_last = true;
    if (s.swing_click_frames > 0)
    {
        s.swing_click_frames--;
        return true;
    }
    return false;
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
        draw_selection(mvp);
        draw_palm_menu(view_proj, tex);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }

    // The Keeper's hand on the right controller, or a pickaxe while
    // possessing an imp or other digger.
    if (s.hands[1].tracked)
    {
        if (s.possessing && s_possession.digger)
            draw_pickaxe(view_proj, s.hands[1]);
        else
            draw_keeper_hand(view_proj, s.hands[1]);
    }

    // Left controller marker: a small block, green while grabbing. While a
    // spell is ready to cast it is a glowing purple orb instead.
    glUseProgram(s.solid_prog);
    glBindVertexArray(s.cube_vao);
    if (s.ray_left && s.hands[0].tracked)
    {
        const Hand& left = s.hands[0];
        const float pulse = 0.75f + 0.25f * std::sin((float)s.frames_rendered * 0.15f);
        // A few turning cubes read as a ball of light.
        for (int k = 0; k < 3; k++)
        {
            const float spin = (float)s.frames_rendered * 0.04f + (float)k * 0.7f;
            const float size = 0.045f * pulse * (1.0f - 0.18f * (float)k);
            const Mat4 orb = view_proj * Mat4::Translation(left.pos + Vec3{0.0f, 0.03f, 0.0f})
                           * Mat4::RotationY(spin) * Mat4::RotationX(spin * 0.7f) * Mat4::Scale({size, size, size});
            glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, orb.m);
            glUniform4f(s.loc_solid_color, 0.55f + 0.15f * k, 0.25f + 0.2f * k, 0.95f, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
    }
    for (const Hand& hand : s.hands)
    {
        if (!hand.tracked || &hand == &s.hands[1] || (s.ray_left && &hand == &s.hands[0]))
            continue;
        // New text waiting: glow yellow, pulsing and a little bigger.
        const bool alert = s.text_alert && &hand == &s.hands[0];
        const float pulse = alert ? 0.5f + 0.5f * std::sin((float)s.frames_rendered * 0.12f) : 0.0f;
        const float grow = 1.0f + 0.25f * pulse;
        const Mat4 mvp = view_proj * Mat4::FromPose(hand.rot, hand.pos) * Mat4::Scale({0.03f * grow, 0.03f * grow, 0.09f * grow});
        glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, mvp.m);
        if (hand.grabbing)
            glUniform4f(s.loc_solid_color, 0.35f, 0.85f, 0.35f, 1.0f);
        else if (alert)
            glUniform4f(s.loc_solid_color, 0.85f + 0.15f * pulse, 0.70f + 0.25f * pulse, 0.10f + 0.25f * pulse, 1.0f);
        else
            glUniform4f(s.loc_solid_color, 0.75f, 0.70f, 0.60f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }

    // Menu screen, with a marker where the finger or ray is on it.
    if (s.menu_shown)
    {
        glUseProgram(s.panel_prog);
        const Mat4 mvp = view_proj * menu_model();
        glUniformMatrix4fv(s.loc_panel_mvp, 1, GL_FALSE, mvp.m);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s.menu_tex);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(s.solid_prog);
        if (s.menu_pointer_valid)
        {
            draw_press_outline(view_proj, s.menu_pointer, s.menu_right, s.menu_up, s.menu_normal,
                               s.menu_flash_time > 0.0f, 0.032f);
            if (s.menu_by_ray)
            {
                const Hand& h = s.hands[1];
                const float length = Length(s.menu_pointer - h.aim_pos);
                const Mat4 ray = view_proj * Mat4::FromPose(h.aim_rot, h.aim_pos)
                               * Mat4::Translation({0, 0, -length * 0.5f}) * Mat4::Scale({0.003f, 0.003f, length});
                glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, ray.m);
                glUniform4f(s.loc_solid_color, 0.55f, 0.15f, 0.09f, 1.0f);
                glDrawArrays(GL_TRIANGLES, 0, 36);
            }
        }
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
    draw_info_box(view_proj);
    draw_cancel_button(view_proj);

    // The fingertip on the wrist panel: an outline instead of the laser.
    if (s_pointer_in.valid && s.panel_finger)
    {
        draw_press_outline(view_proj, s.panel_pointer, s.panel_right, s.panel_up, s.panel_normal,
                           s.panel_flash_time > 0.0f, 0.018f);
    }
    // Pointing ray (right hand, or the left one casting a spell), ending in a
    // marker where it meets the dungeon or the panel. Brighter while the
    // trigger is held.
    else if (s_pointer_in.valid && s.ray_valid && (s_pointer_in.panel_hit || s_pointer_out.hit))
    {
        // Only drawn when it lands on something (the table or the panel).
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
        // Along the hand's -Z (forward) or -X (out of the palm), see render_views().
        const Mat4 ray = s.ray_forward
            ? view_proj * Mat4::FromPose(s.ray_rot, s.ray_origin)
                  * Mat4::Translation({0, 0, -length * 0.5f}) * Mat4::Scale({0.003f, 0.003f, length})
            : view_proj * Mat4::FromPose(s.ray_rot, s.ray_origin)
                  * Mat4::Translation({s.ray_palm_sign * length * 0.5f, 0, 0}) * Mat4::Scale({length, 0.003f, 0.003f});
        glUniformMatrix4fv(s.loc_solid_mvp, 1, GL_FALSE, ray.m);
        if (s.ray_left)
            glUniform4f(s.loc_solid_color, 0.6f * glow, 0.3f * glow, 1.0f * glow, 1.0f);  // spell: purple
        else
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

/** Copies the left eye's image (bound as the read framebuffer's source) into
 *  the desktop window, cropped to the window's shape about its middle, so
 *  people at the PC see what the headset sees. The desktop frame was already
 *  presented there; this replaces it until the swap. */
void mirror_to_window(const EyeTarget& eye)
{
    if (s.window_w <= 0 || s.window_h <= 0 || eye.w <= 0 || eye.h <= 0)
        return;
    const float window_aspect = (float)s.window_w / (float)s.window_h;
    const float eye_aspect = (float)eye.w / (float)eye.h;
    int sx0 = 0, sy0 = 0, sx1 = eye.w, sy1 = eye.h;
    if (window_aspect > eye_aspect)
    {
        // Wider window: all of the eye's width, a band across its middle.
        const int band = (int)((float)eye.w / window_aspect);
        sy0 = (eye.h - band) / 2;
        sy1 = sy0 + band;
    }
    else
    {
        const int band = (int)((float)eye.h * window_aspect);
        sx0 = (eye.w - band) / 2;
        sx1 = sx0 + band;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, eye.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(sx0, sy0, sx1, sy1, 0, 0, s.window_w, s.window_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
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
    const Vec3 head_gaze = Rotate(to_quat(views[0].pose.orientation), {0, 0, -1});
    s.last_head = head;
    update_possession(head, head_gaze);

    // Menu screen: shown while the game shows a menu, placed once in reach.
    s.menu_shown = s_menu_wanted && s.menu_ready;
    if (!s.menu_shown)
        s.menu_placed = false;
    else if (!s.menu_placed)
        place_menu(head, head_gaze);
    s_menu_in = update_menu_touch(s.hands[1]);
    // Won or lost: A or the trigger continues, like the space bar. Not inside a
    // creature, and only after a moment, so the blow that won the level (or a
    // held trigger) does not skip straight past the result.
    {
        if (s_level_decided)
            s.decided_time += s.frame_dt;
        else
            s.decided_time = 0.0f;
        const bool down = s.hands[1].action || s.hands[1].trigger > 0.6f;
        if (s_level_decided && !s.possessing && s.decided_time > 1.5f && down && !s.finish_was_down)
        {
            s_finish_request = true;
            buzz(1, 0.7f, 0.06f);
        }
        s.finish_was_down = down;
    }
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
    // With the wrist panel open the ray points forward along the controller,
    // for clicking the panel. With it closed it comes straight out of the
    // palm of the Keeper's hand (turned like the aim pose, palm towards -X),
    // so a palm-down hand points down at what is under it.
    // Choosing Possess folds the wrist panel away, out of the way of the left
    // hand; it comes back once that is over (cast, cancelled, or the creature left).
    if (s_possess_chosen && !s.possess_was_chosen && !s.panel_hidden)
    {
        s.panel_hidden = true;
        s.panel_auto_hidden = true;
    }
    s.possess_was_chosen = s_possess_chosen;
    // Likewise picking a tool from the panel (a room, spell, trap, door,
    // sell...): the player state changes just after a press on the panel.
    // Tabs and scrolling do not change it, so they leave the panel open.
    if (s.panel_click_time > 0.0f)
        s.panel_click_time -= s.frame_dt;
    if (s_work_state != s.last_work_state)
    {
        if (s.panel_click_time > 0.0f && !s_plain_hand && !s.panel_hidden)
        {
            s.panel_hidden = true;
            s.panel_auto_hidden = true;
        }
        s.last_work_state = s_work_state;
    }
    // Back to the plain hand (placed, cast, cancelled with A, creature left):
    // the panel returns for the next pick.
    if (s.panel_auto_hidden && s_plain_hand && !s_possess_chosen && !s.possessing)
    {
        s.panel_hidden = false;
        s.panel_auto_hidden = false;
    }

    // While possessing there is no ray or panel; the buttons still go through
    // (trigger attacks, A leaves the creature).
    // Turning the left palm up swaps the panel for the palm menu.
    const bool palm_up = s.hands[0].aim_tracked && Rotate(s.hands[0].aim_rot, {1.0f, 0.0f, 0.0f}).y > 0.6f
                      && !s_things.palm.empty();
    s.panel_shown = !s.panel_hidden && s.in_level && !s.possessing && !palm_up && place_panel(head);
    s.ray_valid = pointer.aim_tracked && pointer.tracked && !s.possessing;
    s.ray_forward = s.panel_shown;
    if (s.ray_valid && s.ray_forward)
    {
        s.ray_origin = pointer.aim_pos;
        s.ray_dir = Rotate(pointer.aim_rot, {0.0f, 0.0f, -1.0f});
    }
    else if (s.ray_valid)
    {
        s.ray_origin = pointer.pos + Rotate(pointer.aim_rot, {0.022f, 0.0f, 0.004f});
        s.ray_dir = Rotate(pointer.aim_rot, {-1.0f, 0.0f, 0.0f});
    }
    if (s.ray_valid && s.panel_shown)
        in.panel_hit = ray_hits_panel(s.ray_origin, s.ray_dir, &in.panel_u, &in.panel_v, &s.panel_pointer);
    // The index fingertip on the panel: hovering moves the cursor there,
    // pushing in clicks, as on the menu screen. It takes over from the ray.
    s.panel_finger = false;
    if (s.panel_shown && pointer.tracked)
    {
        const Vec3 tip = index_fingertip(pointer);
        float u, v, depth;
        if (panel_coords(tip, &u, &v, &depth) && depth < 0.08f && depth > -0.06f)
        {
            if (s.panel_touching ? (depth > 0.015f) : (depth < 0.004f))
                s.panel_touching = !s.panel_touching;
            s.panel_finger = true;
            in.panel_hit = true;
            in.panel_u = u;
            in.panel_v = v;
            s.panel_pointer = s.panel_center + s.panel_right * ((u - 0.5f) * s.panel_world_w)
                            + s.panel_up * ((v - 0.5f) * s.panel_world_h);
        }
        else
        {
            s.panel_touching = false;
        }
    }
    else
    {
        s.panel_touching = false;
    }
    s.ray_rot = pointer.aim_rot;
    // A spell is chosen: the left hand casts it, forward along the controller,
    // with its own trigger; the right hand stays free for the panel (to pick
    // another) and its A cancels, as right-click does.
    const Hand& caster = s.hands[0];
    // Off for now: switching hands felt clumsy, so spells cast from the right
    // hand like everything else. The left-hand caster stays for a later try.
    constexpr bool k_left_hand_casts = false;
    s.ray_left = k_left_hand_casts && s_spell_chosen && !in.panel_hit && caster.aim_tracked && s.in_level && !s.possessing;
    s.ray_palm_sign = -1.0f;  // a right palm faces -X
    if (s.ray_left)
    {
        // Out of the left palm, which faces +X: palm down points down.
        s.ray_valid = true;
        s.ray_forward = false;
        s.ray_palm_sign = 1.0f;
        s.ray_origin = caster.pos + Rotate(caster.aim_rot, {-0.022f, 0.0f, 0.004f});
        s.ray_dir = Rotate(caster.aim_rot, {1.0f, 0.0f, 0.0f});
        s.ray_rot = caster.aim_rot;
    }
    const Hand& ray_hand = s.ray_left ? caster : pointer;
    // The palm ray only works the dungeon while it points down at it; a hand
    // held level or on its side (a handshake) leaves the cursor alone and
    // shows no laser. The panel takes the ray at any angle.
    constexpr float k_min_down = 0.25f;  // about 15 degrees below level
    if (s.ray_valid && !s.ray_forward && !in.panel_hit && s.ray_dir.y > -k_min_down)
        s.ray_valid = false;
    if (s.ray_valid && s.in_level)
    {
        const Vec3 origin = stage_to_game(s.ray_origin);
        const Vec3 ahead = stage_to_game(s.ray_origin + s.ray_dir);
        in.valid = true;
        in.ox = origin.x; in.oy = origin.y; in.oz = origin.z;
        in.dx = ahead.x - origin.x; in.dy = ahead.y - origin.y; in.dz = ahead.z - origin.z;
        if (s_select_held ? (ray_hand.trigger < 0.35f) : (ray_hand.trigger > 0.6f))
            s_select_held = !s_select_held;
        in.select = s.panel_finger ? s.panel_touching : s_select_held;
        in.action = pointer.action;
    }
    else if (s.possessing && s.in_level)
    {
        in.valid = true;
        in.buttons_only = true;
        if (s_select_held ? (pointer.trigger < 0.35f) : (pointer.trigger > 0.6f))
            s_select_held = !s_select_held;
        // A digger also strikes with a real swing of the pickaxe.
        const bool swing = s_possession.digger && pointer.tracked && update_pickaxe_swing(pointer, s.frame_dt);
        in.select = s_select_held || swing;
        in.action = pointer.action;
    }
    else
    {
        s_select_held = false;
    }
    if (s.menu_shown)
    {
        // The menu has the hand; nothing goes to the dungeon meanwhile.
        in = PointerInput();
        s.ray_valid = false;
        s.panel_shown = false;
        s_select_held = false;
    }
    // Confirm a press on the panel (finger or ray): a flash and a short buzz.
    if (s.panel_flash_time > 0.0f)
        s.panel_flash_time -= s.frame_dt;
    const bool panel_pressed = in.valid && in.panel_hit && in.select;
    if (panel_pressed && !s.panel_was_pressed)
    {
        s.panel_flash_time = 0.35f;
        buzz(1, 0.6f, 0.04f);
    }
    if (panel_pressed)
        s.panel_click_time = 0.6f;  // a tool picked within this time folds the panel away
    s.panel_was_pressed = panel_pressed;
    // The message box beside the panel: its touch goes to the game in place
    // of the dungeon pointer (see GameThreadSync()).
    s_info_in = update_info_box(head, pointer);
    update_cancel_button(head, pointer);
    if (s.cancel_hover)
        s.ray_valid = false;
    if (s_info_in.valid)
        s.ray_valid = false;
    update_text_alert(head_gaze);
    s_pointer_in = in;
    update_palm_menu(head);
    update_slap(pointer);

    // Ease the right hand into a pointing pose (index out, the rest curled)
    // while its fingertip nears something to press.
    {
        bool near_button = false;
        if (s.hands[1].tracked)
        {
            const Vec3 tip = index_fingertip(s.hands[1]);
            if (s.palm_shown)
                near_button = Length(tip - (s.hands[0].pos + Vec3{0.0f, 0.13f, 0.0f})) < 0.20f;
            float u, v, depth;
            if (!near_button && s.menu_shown && menu_coords(tip, &u, &v, &depth))
                near_button = depth < 0.15f && depth > -0.08f;
            if (!near_button && s.panel_shown && panel_coords(tip, &u, &v, &depth))
                near_button = depth < 0.15f && depth > -0.08f;
            if (!near_button && s.info_shown && info_coords(tip, &u, &v, &depth))
                near_button = depth < 0.15f && depth > -0.08f;
            if (!near_button && s.info_shown && s.info_btn_count > 0)
            {
                // The button row under the box: a band just below its lower edge.
                const Vec3 d = tip - (s.info_center - s.info_up * (s.info_world_h * 0.5f + 0.03f));
                const float du = d.x * s.info_right.x + d.y * s.info_right.y + d.z * s.info_right.z;
                const float dv = d.x * s.info_up.x + d.y * s.info_up.y + d.z * s.info_up.z;
                const float dd = d.x * s.info_normal.x + d.y * s.info_normal.y + d.z * s.info_normal.z;
                near_button = std::fabs(du) < s.info_world_w * 0.5f && std::fabs(dv) < 0.05f && dd < 0.15f && dd > -0.08f;
            }
        }
        const float target = near_button ? 1.0f : 0.0f;
        s.point_pose += (target - s.point_pose) * std::fmin(1.0f, s.frame_dt * 10.0f);
    }

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
        if (i == 0)
            mirror_to_window(eye);

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

    if (s_pause_request)
    {
        s_pause_request = false;
        s_pause_pending = true;  // taken by the input code via VR_ConsumePauseRequest()
    }

    UpdateDungeonSnapshot(s_snapshot);
    s_menu_wanted = MenuScreenWanted(s_snapshot.in_level);
    if (s_snapshot.in_level)
    {
        const bool was_possessing = s_possession.active;
        GetPossessionView(&s_possession);
        // Leaving a creature: back to the plain hand (Possess would otherwise
        // stay chosen, keeping the left hand casting). Sent for a few frames
        // until the game has switched, in case other input that turn wins.
        static int return_to_hand_frames = 0;
        if (was_possessing && !s_possession.active)
            return_to_hand_frames = 10;
        if (return_to_hand_frames > 0)
        {
            return_to_hand_frames--;
            if (SpellChosen())
                ReturnToHand();
            else
                return_to_hand_frames = 0;
        }
        s_things.hide_thing = s_possession.active ? s_possession.thing_idx : 0;
        // Slap: flash what a swat would hit; a swat sends it to the input code.
        s_things.highlight_thing = 0;
        if (s_slap_in.low && !s_possession.active)
        {
            const int target = FindSlapTarget(s_slap_in.x, s_slap_in.y, s_slap_in.radius);
            s_things.highlight_thing = target;
            if (s_slap_in.swipe && target > 0)
            {
                s_slap_pending = target;
                s_slap_landed = true;
            }
        }
        s_look_game = s_look;
        s_look_game.valid = s_look.valid && s_possession.active && s_possession.controlling;
        UpdateThingSnapshot(s_things);
        if (s_menu_wanted)
            ApplyMenuTouch(s_menu_in);  // a menu window over the level has the hand
        else if (s_info_in.valid)
            ApplyInfoTouch(s_info_in, s_info_box);  // the message box beside the panel
        else
            ApplyPointer(s_pointer_in, &s_pointer_out);
        GetPanelInfo(&s_panel_info);
        GetInfoBox(&s_info_box);
        s_spell_chosen = SpellChosen();
        s_possess_chosen = PossessChosen();
        s_level_decided = LevelDecided();
        if (s_finish_request)
        {
            s_finish_request = false;
            s_finish_pending = true;  // taken by VR_ConsumeFinishRequest()
        }
        if (s_cancel_request)
        {
            s_cancel_request = false;
            if (!PlainHand())
                ReturnToHand();  // the red X: drop the room / spell / trap...
        }
        s_work_state = PlayerWorkState();
        s_plain_hand = PlainHand();
        if (s_palm_pick_kind != 0)
        {
            PickUpCreatureOfKind(s_palm_pick_kind);  // tapped on the palm menu
            s_palm_pick_kind = 0;
        }

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
        s_possession = PossessionView();
        s_look_game.valid = false;
        s_spell_chosen = false;
        s_possess_chosen = false;
        s_info_box.visible = false;
        s_level_decided = false;
        s_finish_pending = false;
        ApplyMenuTouch(s_menu_in);  // frontend menus: main menu, campaign map, ...
    }
}

void RenderThreadFrame(GLWorldViewRenderer& world, unsigned int screen_fbo, int screen_w, int screen_h,
                       int window_w, int window_h)
{
    if (!vr_requested || s.failed)
        return;
    s.window_w = window_w;
    s.window_h = window_h;
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
    capture_menu((GLuint)screen_fbo, screen_w, screen_h);
    capture_info((GLuint)screen_fbo, screen_w, screen_h);

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

int VR_ConsumeFinishRequest(void)
{
    if (!vr_requested || !VR::s_finish_pending)
        return 0;
    VR::s_finish_pending = false;
    return 1;
}

int VR_ConsumeSlapTarget(void)
{
    if (!vr_requested || VR::s_slap_pending <= 0)
        return 0;
    const int target = VR::s_slap_pending;
    VR::s_slap_pending = 0;
    return target;
}

int VR_ConsumePauseRequest(void)
{
    if (!vr_requested || !VR::s_pause_pending)
        return 0;
    VR::s_pause_pending = false;
    return 1;
}

int VR_GetPossessionControls(int* yaw, int* pitch, int* move_x, int* move_y)
{
    if (!vr_requested || !VR::s_look_game.valid)
        return 0;
    *yaw = VR::s_look_game.yaw;
    *pitch = VR::s_look_game.pitch;
    *move_x = VR::s_look_game.move_x;
    *move_y = VR::s_look_game.move_y;
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
void RenderThreadFrame(GLWorldViewRenderer&, unsigned int, int, int, int, int) {}
bool IsSessionRunning() { return false; }
void RenderThreadShutdown() {}
} // namespace VR

int VR_GetSoundListener(int*, int*, int*, int*) { return 0; }
int VR_IsHeadsetActive(void) { return 0; }
int VR_ConsumePauseRequest(void) { return 0; }
int VR_ConsumeSlapTarget(void) { return 0; }
int VR_ConsumeFinishRequest(void) { return 0; }
int VR_GetPossessionControls(int*, int*, int*, int*) { return 0; }

#include "post_inc.h"

#endif // KFX_VR
