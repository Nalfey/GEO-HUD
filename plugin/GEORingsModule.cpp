#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>

#include "SceneHook.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using lua_State = struct lua_State;
using lua_CFunction = int(__cdecl*)(lua_State*);

using fn_createtable = void(__cdecl*)(lua_State*, int, int);
using fn_pushcclosure = void(__cdecl*)(lua_State*, lua_CFunction, int);
using fn_setfield = void(__cdecl*)(lua_State*, int, char const*);
using fn_pushvalue = void(__cdecl*)(lua_State*, int);
using fn_pushstring = void(__cdecl*)(lua_State*, char const*);
using fn_gettop = int(__cdecl*)(lua_State*);
using fn_tonumber = double(__cdecl*)(lua_State*, int);
using fn_pushnumber = void(__cdecl*)(lua_State*, double);

constexpr int kGlobalsIndex = -10002;

struct LuaApi {
    fn_createtable createtable = nullptr;
    fn_pushcclosure pushcclosure = nullptr;
    fn_setfield setfield = nullptr;
    fn_pushvalue pushvalue = nullptr;
    fn_pushstring pushstring = nullptr;
    fn_gettop gettop = nullptr;
    fn_tonumber tonumber = nullptr;
    fn_pushnumber pushnumber = nullptr;

    bool ready() const {
        return createtable && pushcclosure && setfield && pushvalue
            && pushstring && gettop && tonumber && pushnumber;
    }
};

LuaApi g_lua {};
HMODULE g_self = nullptr;

struct Position {
    float east = 0.0f;
    float north = 0.0f;
    float height = 0.0f;
};

struct DrawVertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
};

struct MotionState {
    Position pos {};
    Position velocity {};
    DWORD last_ms = 0;
    bool initialized = false;
};

struct Ring {
    DWORD index = 0;
    Position centre;
    float radius = 0.0f;
    DWORD color = 0;
    bool active = false;
};

constexpr int kMaxRings = 50;
constexpr int ring_slices_max_ = 48;
constexpr int max_batch_vertices_ = 32768;
constexpr float kGroundClearance = 0.05f;
constexpr float kSmoothBlend = 0.72f;
constexpr float kTwoPi = 6.28318530718f;
constexpr float kRearFade = 0.120f;
constexpr float kRangeRearFade = 0.220f;
constexpr float kFadeCurve = 0.500f;
constexpr float kPulseDepth = 0.200f;
constexpr float kGlowAlpha = 0.200f;
constexpr float kPlayerBodyHeight = 1.72f;
constexpr float kPlayerOccludeRadius = 0.42f;
constexpr unsigned kPulsePeriodMs = 800u;
constexpr DWORD kMaxIndex = 0x900;
// entity+0x004 is the client's *predicted* position and leads the mesh while a
// mob runs. The display object's nameplate base is where the model is actually
// drawn, so that is preferred and 0x004 is only a fallback for unrendered
// entities. Layout per Windower's libraries/memory/types.lua.
constexpr std::uintptr_t kEntityDisplayPos = 0x004;
constexpr std::uintptr_t kEntityDisplayPtr = 0x0A0;
constexpr std::uintptr_t kDisplayNameplateBase = 0x678;

bool page_executable(DWORD protect);

Ring g_rings[kMaxRings] {};
Ring g_staging[kMaxRings] {};
MotionState g_motion[kMaxIndex] {};
CRITICAL_SECTION g_ring_lock {};
bool g_ring_lock_ready = false;
volatile bool g_samples_fresh = false;
bool g_draw_samples_fresh = false;
volatile int g_ring_count = 0;
int g_staging_count = 0;
DWORD g_player_index = 0;
Position g_player_pos {};
bool g_player_has_pos = false;
bool g_player_occluder_valid = false;
bool g_soft_player_cutout = false;
float g_player_foot_x = 0.0f;
float g_player_foot_y = 0.0f;
float g_player_head_x = 0.0f;
float g_player_head_y = 0.0f;
float g_player_half_width = 18.0f;
int g_active_slices = ring_slices_max_;
bool g_draw_outer_glow = true;
bool g_colorblind = false;
Ring g_chant {};
constexpr int kMaxRangeRings = 8;
Ring g_range[kMaxRangeRings] {};
Ring g_range_staging[kMaxRangeRings] {};
int g_range_count = 0;
int g_range_staging_count = 0;

struct CompassMark {
    float x = -1.0f;
    float y = -1.0f;
    float z = 0.0f;
    float rhw = 1.0f;
    bool ok = false;
};
CompassMark g_compass[4] {}; // N, E, S, W screen positions from last draw

// The scene hook owns the draw_scene patch; this module only registers a
// callback with it. See ../SceneHook/SceneHook.md.
SceneBus* g_bus = nullptr;
int g_bus_slot = -1;
volatile bool g_hooked = false;
volatile unsigned long g_frames = 0;
volatile unsigned long g_draws = 0;
std::uintptr_t g_renderer = 0;
std::uintptr_t g_device = 0;
volatile bool g_discovery_done = false;
unsigned long g_device_attempts = 0;
constexpr std::size_t kRendererScanWindow = 0x8000;
constexpr std::size_t kDeviceVtableEntries = 90;
constexpr unsigned long kDeviceRetryFrames = 30;
constexpr unsigned long kMaxDeviceAttempts = 240;
char g_status[192] = "idle";

bool page_readable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) {
        return false;
    }

    switch (protect & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool span_readable(std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION region {};
    if (!VirtualQuery(reinterpret_cast<void const*>(address), &region, sizeof(region))) {
        return false;
    }

    if (region.State != MEM_COMMIT || !page_readable(region.Protect)) {
        return false;
    }

    std::uintptr_t const low = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return address >= low && address + size <= low + region.RegionSize;
}

bool module_range(char const* name, std::uintptr_t& base, std::size_t& size) {
    base = 0;
    size = 0;

    HMODULE module = GetModuleHandleA(name);
    if (!module) {
        return false;
    }

    base = reinterpret_cast<std::uintptr_t>(module);
    if (!span_readable(base, sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }

    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    std::uintptr_t const nt_address = base + static_cast<std::uintptr_t>(dos->e_lfanew);
    if (!span_readable(nt_address, sizeof(IMAGE_NT_HEADERS32))) {
        return false;
    }

    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(nt_address);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    size = nt->OptionalHeader.SizeOfImage;
    return size != 0;
}

std::uintptr_t scan_module(char const* name, unsigned char const* pattern,
    char const* mask, std::size_t length) {
    std::uintptr_t base = 0;
    std::size_t image = 0;
    if (!module_range(name, base, image)) {
        return 0;
    }

    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(
        base + static_cast<std::uintptr_t>(dos->e_lfanew));

    auto const* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        std::uintptr_t const start = base + section->VirtualAddress;
        std::size_t const span = section->Misc.VirtualSize;
        if (span <= length || !span_readable(start, span)) {
            continue;
        }

        auto const* bytes = reinterpret_cast<unsigned char const*>(start);
        for (std::size_t offset = 0; offset + length <= span; ++offset) {
            bool hit = true;
            for (std::size_t j = 0; j < length; ++j) {
                if (mask[j] != '?' && bytes[offset + j] != pattern[j]) {
                    hit = false;
                    break;
                }
            }

            if (hit) {
                return start + offset;
            }
        }
    }

    return 0;
}

void* vtable_slot(std::uintptr_t object, int index) {
    if (!span_readable(object, sizeof(std::uintptr_t))) {
        return nullptr;
    }

    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, reinterpret_cast<void const*>(object), sizeof(vtable));

    std::uintptr_t const entry = vtable + static_cast<std::uintptr_t>(index) * sizeof(std::uintptr_t);
    if (!span_readable(entry, sizeof(std::uintptr_t))) {
        return nullptr;
    }

    std::uintptr_t address = 0;
    std::memcpy(&address, reinterpret_cast<void const*>(entry), sizeof(address));
    return reinterpret_cast<void*>(address);
}

using fn_get_device = long(__stdcall*)(void*, void**);
using fn_release = unsigned long(__stdcall*)(void*);
using fn_get_transform = long(__stdcall*)(void*, DWORD, D3DMATRIX*);
using fn_get_viewport = long(__stdcall*)(void*, D3DVIEWPORT8*);
using fn_get_render_state = long(__stdcall*)(void*, DWORD, DWORD*);
using fn_set_render_state = long(__stdcall*)(void*, DWORD, DWORD);
using fn_get_vertex_shader = long(__stdcall*)(void*, DWORD*);
using fn_set_vertex_shader = long(__stdcall*)(void*, DWORD);
using fn_get_texture = long(__stdcall*)(void*, DWORD, void**);
using fn_set_texture = long(__stdcall*)(void*, DWORD, void*);
using fn_draw_up = long(__stdcall*)(void*, DWORD, unsigned, void const*, unsigned);

void* d3d_device_ = nullptr;

long dev_GetTransform(DWORD state, D3DMATRIX* out) {
    auto f = reinterpret_cast<fn_get_transform>(vtable_slot(g_device, 38));
    return f ? f(d3d_device_, state, out) : -1;
}

long dev_GetViewport(D3DVIEWPORT8* out) {
    auto f = reinterpret_cast<fn_get_viewport>(vtable_slot(g_device, 41));
    return f ? f(d3d_device_, out) : -1;
}

long dev_GetRenderState(DWORD state, DWORD* out) {
    auto f = reinterpret_cast<fn_get_render_state>(vtable_slot(g_device, 51));
    return f ? f(d3d_device_, state, out) : -1;
}

long dev_SetRenderState(DWORD state, DWORD value) {
    auto f = reinterpret_cast<fn_set_render_state>(vtable_slot(g_device, 50));
    return f ? f(d3d_device_, state, value) : -1;
}

long dev_GetVertexShader(DWORD* out) {
    auto f = reinterpret_cast<fn_get_vertex_shader>(vtable_slot(g_device, 77));
    return f ? f(d3d_device_, out) : -1;
}

long dev_SetVertexShader(DWORD value) {
    auto f = reinterpret_cast<fn_set_vertex_shader>(vtable_slot(g_device, 76));
    return f ? f(d3d_device_, value) : -1;
}

long dev_GetTexture(DWORD stage, IDirect3DBaseTexture8** out) {
    auto f = reinterpret_cast<fn_get_texture>(vtable_slot(g_device, 60));
    return f ? f(d3d_device_, stage, reinterpret_cast<void**>(out)) : -1;
}

long dev_SetTexture(DWORD stage, void* texture) {
    auto f = reinterpret_cast<fn_set_texture>(vtable_slot(g_device, 61));
    return f ? f(d3d_device_, stage, texture) : -1;
}

long dev_DrawPrimitiveUP(DWORD type, unsigned count, void const* data, unsigned stride) {
    auto f = reinterpret_cast<fn_draw_up>(vtable_slot(g_device, 72));
    return f ? f(d3d_device_, type, count, data, stride) : -1;
}

D3DMATRIX cached_view_ {};
D3DMATRIX cached_projection_ {};
D3DMATRIX cached_view_projection_ {};
bool projection_matrices_valid_ = false;

DWORD saved_shader_ = 0;
DWORD saved_alpha_ = 0;
DWORD saved_src_ = 0;
DWORD saved_dest_ = 0;
DWORD saved_z_ = 0;
DWORD saved_zfunc_ = 0;
DWORD saved_zwrite_ = 0;
DWORD saved_lighting_ = 0;
DWORD saved_cull_ = 0;
IDirect3DBaseTexture8* saved_texture_ = nullptr;
bool draw_state_active_ = false;

DrawVertex batch_vertices_[max_batch_vertices_] {};
int batch_vertex_count_ = 0;

bool refresh_projection_matrices() {
    if (!d3d_device_) {
        return false;
    }

    if (dev_GetTransform(2, &cached_view_) < 0 || dev_GetTransform(3, &cached_projection_) < 0) {
        projection_matrices_valid_ = false;
        return false;
    }

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            cached_view_projection_.m[row][column] =
                cached_view_.m[row][0] * cached_projection_.m[0][column]
                + cached_view_.m[row][1] * cached_projection_.m[1][column]
                + cached_view_.m[row][2] * cached_projection_.m[2][column]
                + cached_view_.m[row][3] * cached_projection_.m[3][column];
        }
    }

    projection_matrices_valid_ = true;
    return true;
}
float ring_cos_[ring_slices_max_ + 1] {};
float ring_sin_[ring_slices_max_ + 1] {};

void build_tables() {
    for (int i = 0; i <= ring_slices_max_; ++i) {
        float const angle = 6.28318530718f * static_cast<float>(i) / static_cast<float>(ring_slices_max_);
        ring_cos_[i] = std::cos(angle);
        ring_sin_[i] = std::sin(angle);
    }
}

void flush_batch();

DWORD scale_alpha(DWORD color, float scale) {
    const float alpha = static_cast<float>((color >> 24) & 0xFF) * scale;
    const DWORD clamped = static_cast<DWORD>(std::fmax(0.0f, std::fmin(255.0f, alpha)) + 0.5f);
    return (clamped << 24) | (color & 0x00FFFFFF);
}

DWORD mix_rgb(DWORD color, DWORD other, float t) {
    if (t <= 0.0f) {
        return color;
    }
    if (t >= 1.0f) {
        return (color & 0xFF000000u) | (other & 0x00FFFFFFu);
    }
    auto channel = [](DWORD value, int shift) {
        return static_cast<int>((value >> shift) & 0xFF);
    };
    int const r = channel(color, 16) + static_cast<int>((channel(other, 16) - channel(color, 16)) * t + 0.5f);
    int const g = channel(color, 8) + static_cast<int>((channel(other, 8) - channel(color, 8)) * t + 0.5f);
    int const b = channel(color, 0) + static_cast<int>((channel(other, 0) - channel(color, 0)) * t + 0.5f);
    return (color & 0xFF000000u)
        | (static_cast<DWORD>(r) << 16)
        | (static_cast<DWORD>(g) << 8)
        | static_cast<DWORD>(b);
}

DWORD retint_range(DWORD color) {
    float r = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
    float g = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
    float b = static_cast<float>(color & 0xFF) / 255.0f;
    float const maxc = std::fmax(r, std::fmax(g, b));
    float const minc = std::fmin(r, std::fmin(g, b));
    float const delta = maxc - minc;
    float h = 0.0f;
    float s = 0.0f;
    if (maxc > 0.0001f) {
        s = delta / maxc;
        if (delta > 0.0001f) {
            if (maxc == r) {
                h = (g - b) / delta + (g < b ? 6.0f : 0.0f);
            } else if (maxc == g) {
                h = (b - r) / delta + 2.0f;
            } else {
                h = (r - g) / delta + 4.0f;
            }
            h /= 6.0f;
        }
    }
    s = std::fmin(1.0f, s * 1.3f);
    float const v = maxc * 0.5f;
    float const hue = h * 6.0f;
    int const sector = static_cast<int>(hue) % 6;
    float const f = hue - std::floor(hue);
    float const p = v * (1.0f - s);
    float const q = v * (1.0f - f * s);
    float const t = v * (1.0f - (1.0f - f) * s);
    switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    auto byte = [](float c) {
        return static_cast<DWORD>(std::fmax(0.0f, std::fmin(255.0f, c * 255.0f)) + 0.5f);
    };
    return (color & 0xFF000000u) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

DWORD boost_chant_letter(DWORD color) {
    int r = static_cast<int>((color >> 16) & 0xFF);
    int g = static_cast<int>((color >> 8) & 0xFF);
    int b = static_cast<int>(color & 0xFF);
    r = (r * 2 + 255) / 3 + 40;
    g = (g * 2 + 255) / 3 + 40;
    b = (b * 2 + 255) / 3 + 40;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u
        | (static_cast<DWORD>(r) << 16)
        | (static_cast<DWORD>(g) << 8)
        | static_cast<DWORD>(b);
}

float g_fade_hub = 0.0f;
float g_fade_span = 1.0f;
float g_fade_floor = kRearFade;

float distance_fade(float distance) {
    if (g_fade_span <= 0.0f) {
        return 1.0f;
    }

    float t = (distance - g_fade_hub) / g_fade_span;
    t = std::fmax(0.0f, std::fmin(1.0f, t));
    return 1.0f - (1.0f - g_fade_floor) * std::pow(t, kFadeCurve);
}

float point_to_segment(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float length_sq = dx * dx + dy * dy;
    float t = 0.0f;
    if (length_sq > 0.0001f) {
        t = ((px - ax) * dx + (py - ay) * dy) / length_sq;
        t = std::fmax(0.0f, std::fmin(1.0f, t));
    }
    const float qx = ax + t * dx;
    const float qy = ay + t * dy;
    return std::hypot(px - qx, py - qy);
}

float player_cover(float screen_x, float screen_y) {
    if (!g_soft_player_cutout || !g_player_occluder_valid) {
        return 0.0f;
    }
    const float distance = point_to_segment(screen_x, screen_y,
        g_player_foot_x, g_player_foot_y, g_player_head_x, g_player_head_y);
    const float inner = g_player_half_width * 0.70f;
    const float outer = g_player_half_width * 1.40f;
    if (distance <= inner) {
        return 1.0f;
    }
    if (distance >= outer) {
        return 0.0f;
    }
    const float t = (outer - distance) / (outer - inner);
    return t * t * (3.0f - 2.0f * t);
}

DWORD shaded_color(DWORD color, float screen_x, float screen_y, float distance) {
    return scale_alpha(color, distance_fade(distance) * (1.0f - player_cover(screen_x, screen_y)));
}

void update_draw_quality() {
    if (g_ring_count >= 41) {
        g_active_slices = 24;
        g_draw_outer_glow = false;
    } else if (g_ring_count >= 26) {
        g_active_slices = 32;
        g_draw_outer_glow = true;
    } else {
        g_active_slices = ring_slices_max_;
        g_draw_outer_glow = true;
    }
}

// FFXI world coordinates are x/y on the ground plane with z as height; D3D wants
// x/z on the ground with y up.
bool world_to_screen(const Position& point, const D3DVIEWPORT8& viewport,
    float& screen_x, float& screen_y, float& screen_rhw, float& screen_z) {
    if (!projection_matrices_valid_) {
        return false;
    }

    const D3DMATRIX& vp = cached_view_projection_;
    const float d3d_x = point.east;
    const float d3d_y = point.height;
    const float d3d_z = point.north;

    const float clip_x = d3d_x * vp.m[0][0] + d3d_y * vp.m[1][0]
        + d3d_z * vp.m[2][0] + vp.m[3][0];
    const float clip_y = d3d_x * vp.m[0][1] + d3d_y * vp.m[1][1]
        + d3d_z * vp.m[2][1] + vp.m[3][1];
    const float clip_z = d3d_x * vp.m[0][2] + d3d_y * vp.m[1][2]
        + d3d_z * vp.m[2][2] + vp.m[3][2];
    const float clip_w = d3d_x * vp.m[0][3] + d3d_y * vp.m[1][3]
        + d3d_z * vp.m[2][3] + vp.m[3][3];

    if (std::fabs(clip_w) <= 0.0001f) {
        return false;
    }

    const float ndc_x = clip_x / clip_w;
    const float ndc_y = clip_y / clip_w;
    if (clip_w < 0.0f || ndc_x < -4.0f || ndc_x > 4.0f || ndc_y < -4.0f || ndc_y > 4.0f) {
        return false;
    }

    screen_x = static_cast<float>(viewport.X) + (ndc_x + 1.0f) * static_cast<float>(viewport.Width) * 0.5f;
    screen_y = static_cast<float>(viewport.Y) + (1.0f - ndc_y) * static_cast<float>(viewport.Height) * 0.5f;
    screen_rhw = 1.0f / clip_w;
    screen_z = std::fmax(0.0f, std::fmin(1.0f, clip_z / clip_w));
    return true;
}

bool world_to_screen(const Position& point, const D3DVIEWPORT8& viewport,
    float& screen_x, float& screen_y, float& screen_rhw) {
    float discarded = 0.0f;
    return world_to_screen(point, viewport, screen_x, screen_y, screen_rhw, discarded);
}

bool begin_draw_state() {
    if (draw_state_active_) {
        return true;
    }

    if (!d3d_device_) {
        return false;
    }

    saved_texture_ = nullptr;
    dev_GetVertexShader(&saved_shader_);
    dev_GetRenderState(D3DRS_ALPHABLENDENABLE, &saved_alpha_);
    dev_GetRenderState(D3DRS_SRCBLEND, &saved_src_);
    dev_GetRenderState(D3DRS_DESTBLEND, &saved_dest_);
    dev_GetRenderState(D3DRS_ZENABLE, &saved_z_);
    dev_GetRenderState(D3DRS_LIGHTING, &saved_lighting_);
    dev_GetRenderState(D3DRS_CULLMODE, &saved_cull_);
    dev_GetRenderState(D3DRS_ZFUNC, &saved_zfunc_);
    dev_GetRenderState(D3DRS_ZWRITEENABLE, &saved_zwrite_);
    dev_GetTexture(0, &saved_texture_);

    dev_SetTexture(0, nullptr);
    dev_SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev_SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev_SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    // Inside draw_scene the depth buffer is still bound, so the ring is cut
    // out by the character or mob standing in it. Depth writes stay off.
    dev_SetRenderState(D3DRS_ZENABLE, TRUE);
    dev_SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev_SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev_SetRenderState(D3DRS_LIGHTING, FALSE);
    dev_SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev_SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev_SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    draw_state_active_ = true;
    return true;
}

void end_draw_state() {
    if (!draw_state_active_ || !d3d_device_) {
        return;
    }

    dev_SetTexture(0, saved_texture_);
    if (saved_texture_) {
        saved_texture_->Release();
        saved_texture_ = nullptr;
    }
    dev_SetRenderState(D3DRS_ALPHABLENDENABLE, saved_alpha_);
    dev_SetRenderState(D3DRS_SRCBLEND, saved_src_);
    dev_SetRenderState(D3DRS_DESTBLEND, saved_dest_);
    dev_SetRenderState(D3DRS_ZENABLE, saved_z_);
    dev_SetRenderState(D3DRS_LIGHTING, saved_lighting_);
    dev_SetRenderState(D3DRS_CULLMODE, saved_cull_);
    dev_SetRenderState(D3DRS_ZFUNC, saved_zfunc_);
    dev_SetRenderState(D3DRS_ZWRITEENABLE, saved_zwrite_);
    dev_SetVertexShader(saved_shader_);
    draw_state_active_ = false;
}

void append_batch(const DrawVertex* vertices, int count) {
    if (count <= 0 || count > max_batch_vertices_) {
        return;
    }

    if (batch_vertex_count_ + count > max_batch_vertices_) {
        flush_batch();
    }

    std::memcpy(batch_vertices_ + batch_vertex_count_, vertices,
        static_cast<std::size_t>(count) * sizeof(DrawVertex));
    batch_vertex_count_ += count;
}

void flush_batch() {
    if (batch_vertex_count_ < 3 || !d3d_device_) {
        batch_vertex_count_ = 0;
        return;
    }

    dev_DrawPrimitiveUP(D3DPT_TRIANGLELIST,
        static_cast<UINT>(batch_vertex_count_ / 3), batch_vertices_, sizeof(DrawVertex));
    batch_vertex_count_ = 0;
}


void draw_ground_ring(const Position& centre, float radius,
    float band_width, const D3DVIEWPORT8& viewport, DWORD color) {
    if (radius <= 0.0f) {
        return;
    }

    const int slices = g_active_slices;
    const float inner_radius = std::fmax(radius - band_width * 0.5f, 0.01f);
    const float outer_radius = radius + band_width * 0.5f;

    const Position hub {centre.east, centre.north, centre.height - kGroundClearance};

    float hub_x = 0.0f;
    float hub_y = 0.0f;
    float hub_rhw = 1.0f;
    if (!world_to_screen(hub, viewport, hub_x, hub_y, hub_rhw) || hub_rhw <= 0.0f) {
        return;
    }

    float inner_z[ring_slices_max_ + 1] {};
    float outer_z[ring_slices_max_ + 1] {};
    float inner_w[ring_slices_max_ + 1] {};
    float outer_w[ring_slices_max_ + 1] {};
    float inner_x[ring_slices_max_ + 1] {};
    float inner_y[ring_slices_max_ + 1] {};
    float outer_x[ring_slices_max_ + 1] {};
    float outer_y[ring_slices_max_ + 1] {};
    DWORD inner_color[ring_slices_max_ + 1] {};
    DWORD outer_color[ring_slices_max_ + 1] {};
    bool resolved[ring_slices_max_ + 1] {};

    for (int i = 0; i <= slices; ++i) {
        float cos_a = 0.0f;
        float sin_a = 0.0f;
        if (slices == ring_slices_max_) {
            cos_a = ring_cos_[i];
            sin_a = ring_sin_[i];
        } else {
            const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(slices);
            cos_a = std::cos(angle);
            sin_a = std::sin(angle);
        }

        float inner_rhw = 1.0f;
        float outer_rhw = 1.0f;

        const Position near_point {hub.east + inner_radius * cos_a,
            hub.north + inner_radius * sin_a, hub.height};
        const Position far_point {hub.east + outer_radius * cos_a,
            hub.north + outer_radius * sin_a, hub.height};

        const bool inner_ok = world_to_screen(near_point, viewport, inner_x[i], inner_y[i],
            inner_rhw, inner_z[i]);
        const bool outer_ok = world_to_screen(far_point, viewport, outer_x[i], outer_y[i],
            outer_rhw, outer_z[i]);
        inner_w[i] = inner_rhw;
        outer_w[i] = outer_rhw;

        resolved[i] = inner_ok && outer_ok && inner_rhw > 0.0f && outer_rhw > 0.0f;
        if (!resolved[i]) {
            continue;
        }

        inner_color[i] = shaded_color(color, inner_x[i], inner_y[i], 1.0f / inner_rhw);
        outer_color[i] = shaded_color(color, outer_x[i], outer_y[i], 1.0f / outer_rhw);
    }

    DrawVertex quad[6] {};
    for (int i = 0; i < slices; ++i) {
        if (!resolved[i] || !resolved[i + 1]) {
            continue;
        }

        if (((inner_color[i] | outer_color[i] | inner_color[i + 1] | outer_color[i + 1])
            & 0xFF000000u) == 0) {
            continue;
        }

        quad[0] = {inner_x[i], inner_y[i], inner_z[i], inner_w[i], inner_color[i]};
        quad[1] = {outer_x[i], outer_y[i], outer_z[i], outer_w[i], outer_color[i]};
        quad[2] = {inner_x[i + 1], inner_y[i + 1], inner_z[i + 1], inner_w[i + 1], inner_color[i + 1]};
        quad[3] = {inner_x[i + 1], inner_y[i + 1], inner_z[i + 1], inner_w[i + 1], inner_color[i + 1]};
        quad[4] = {outer_x[i], outer_y[i], outer_z[i], outer_w[i], outer_color[i]};
        quad[5] = {outer_x[i + 1], outer_y[i + 1], outer_z[i + 1], outer_w[i + 1], outer_color[i + 1]};

        append_batch(quad, 6);
    }
}

bool emit_quad(const Position& a, const Position& b, const Position& c, const Position& d,
    const D3DVIEWPORT8& viewport, DWORD color) {
    const Position corners[4] = {a, b, c, d};
    DrawVertex screen[4] {};

    for (int i = 0; i < 4; ++i) {
        float rhw = 1.0f;
        float depth = 0.0f;
        if (!world_to_screen(corners[i], viewport, screen[i].x, screen[i].y, rhw, depth)
            || rhw <= 0.0f) {
            return false;
        }
        screen[i].z = depth;
        screen[i].rhw = rhw;
        screen[i].color = shaded_color(color, screen[i].x, screen[i].y, 1.0f / rhw);
    }

    DrawVertex quad[6] = {
        screen[0], screen[1], screen[2],
        screen[0], screen[2], screen[3],
    };
    append_batch(quad, 6);
    return true;
}

void emit_screen_quad(float x0, float y0, float x1, float y1,
    float z, float rhw, DWORD color) {
    DrawVertex quad[6] = {
        {x0, y0, z, rhw, color},
        {x1, y0, z, rhw, color},
        {x0, y1, z, rhw, color},
        {x0, y1, z, rhw, color},
        {x1, y0, z, rhw, color},
        {x1, y1, z, rhw, color},
    };
    append_batch(quad, 6);
}

void emit_bar(const Position& origin, float dir_east, float dir_north,
    float half_len, float half_w, const D3DVIEWPORT8& viewport, DWORD color) {
    const float len = std::hypot(dir_east, dir_north);
    if (len <= 0.0001f || half_len <= 0.0f || half_w <= 0.0f) {
        return;
    }

    const float dx = dir_east / len;
    const float dy = dir_north / len;
    const float px = -dy;
    const float py = dx;
    const float h = origin.height;
    emit_quad(
        {origin.east + dx * half_len + px * half_w,
            origin.north + dy * half_len + py * half_w, h},
        {origin.east + dx * half_len - px * half_w,
            origin.north + dy * half_len - py * half_w, h},
        {origin.east - dx * half_len - px * half_w,
            origin.north - dy * half_len - py * half_w, h},
        {origin.east - dx * half_len + px * half_w,
            origin.north - dy * half_len + py * half_w, h},
        viewport, color);
}

void draw_marker_annulus(const Position& origin, float radius, float width,
    const D3DVIEWPORT8& viewport, DWORD color) {
    constexpr int slices = 12;
    const float inner_radius = std::fmax(radius - width * 0.5f, 0.01f);
    const float outer_radius = radius + width * 0.5f;
    const Position hub {origin.east, origin.north, origin.height - kGroundClearance};

    float inner_x[slices + 1] {};
    float inner_y[slices + 1] {};
    float inner_z[slices + 1] {};
    float inner_w[slices + 1] {};
    float outer_x[slices + 1] {};
    float outer_y[slices + 1] {};
    float outer_z[slices + 1] {};
    float outer_w[slices + 1] {};
    DWORD inner_color[slices + 1] {};
    DWORD outer_color[slices + 1] {};
    bool resolved[slices + 1] {};

    for (int i = 0; i <= slices; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(slices);
        const float cos_a = std::cos(angle);
        const float sin_a = std::sin(angle);
        float inner_rhw = 1.0f;
        float outer_rhw = 1.0f;
        const Position near_point {hub.east + inner_radius * cos_a,
            hub.north + inner_radius * sin_a, hub.height};
        const Position far_point {hub.east + outer_radius * cos_a,
            hub.north + outer_radius * sin_a, hub.height};
        const bool inner_ok = world_to_screen(near_point, viewport, inner_x[i], inner_y[i],
            inner_rhw, inner_z[i]);
        const bool outer_ok = world_to_screen(far_point, viewport, outer_x[i], outer_y[i],
            outer_rhw, outer_z[i]);
        inner_w[i] = inner_rhw;
        outer_w[i] = outer_rhw;
        resolved[i] = inner_ok && outer_ok && inner_rhw > 0.0f && outer_rhw > 0.0f;
        if (!resolved[i]) {
            continue;
        }
        inner_color[i] = shaded_color(color, inner_x[i], inner_y[i], 1.0f / inner_rhw);
        outer_color[i] = shaded_color(color, outer_x[i], outer_y[i], 1.0f / outer_rhw);
    }

    DrawVertex quad[6] {};
    for (int i = 0; i < slices; ++i) {
        if (!resolved[i] || !resolved[i + 1]) {
            continue;
        }
        quad[0] = {inner_x[i], inner_y[i], inner_z[i], inner_w[i], inner_color[i]};
        quad[1] = {outer_x[i], outer_y[i], outer_z[i], outer_w[i], outer_color[i]};
        quad[2] = {inner_x[i + 1], inner_y[i + 1], inner_z[i + 1], inner_w[i + 1], inner_color[i + 1]};
        quad[3] = {inner_x[i + 1], inner_y[i + 1], inner_z[i + 1], inner_w[i + 1], inner_color[i + 1]};
        quad[4] = {outer_x[i], outer_y[i], outer_z[i], outer_w[i], outer_color[i]};
        quad[5] = {outer_x[i + 1], outer_y[i + 1], outer_z[i + 1], outer_w[i + 1], outer_color[i + 1]};
        append_batch(quad, 6);
    }
}

void draw_colorblind_markers(const Position& centre, float radius,
    DWORD ring_color, const D3DVIEWPORT8& viewport) {
    const int red = static_cast<int>((ring_color >> 16) & 0xFF);
    const int green = static_cast<int>((ring_color >> 8) & 0xFF);
    const bool is_ok = green > red;
    const float size = std::fmax(0.16f, std::fmin(0.40f, radius * 0.48f));
    constexpr DWORD kOutline = 0xFF000000;
    constexpr DWORD kFill = 0xFFFFFFFF;
    const float height = centre.height - kGroundClearance;

    for (int i = 0; i < 6; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / 6.0f;
        const Position seat {
            centre.east + radius * std::cos(angle),
            centre.north + radius * std::sin(angle),
            height,
        };
        if (is_ok) {
            draw_marker_annulus(seat, size * 0.50f, size * 0.40f, viewport, kOutline);
            draw_marker_annulus(seat, size * 0.50f, size * 0.18f, viewport, kFill);
        } else {
            const float half_len = size * 0.70f;
            emit_bar(seat, 1.0f, 1.0f, half_len, size * 0.20f, viewport, kOutline);
            emit_bar(seat, 1.0f, -1.0f, half_len, size * 0.20f, viewport, kOutline);
            emit_bar(seat, 1.0f, 1.0f, half_len * 0.90f, size * 0.09f, viewport, kFill);
            emit_bar(seat, 1.0f, -1.0f, half_len * 0.90f, size * 0.09f, viewport, kFill);
        }
    }
}

std::uintptr_t g_entity_array = 0;
bool g_entity_array_resolved = false;

// mov edx,[esi+0xC] / mov eax,[edx+ebp] / mov eax,[eax*4+imm32] -- the trailing
// imm32 is the array base itself, not a pointer to it.
std::uintptr_t entity_array() {
    if (g_entity_array_resolved) {
        return g_entity_array;
    }

    g_entity_array_resolved = true;

    static unsigned char const pattern[] = {
        0x8B, 0x56, 0x0C, 0x8B, 0x04, 0x2A, 0x8B, 0x04, 0x85,
    };
    static char const mask[] = "xxxxxxxxx";

    std::uintptr_t const match = scan_module("FFXiMain.dll", pattern, mask, sizeof(pattern));
    if (match == 0) {
        return 0;
    }

    std::uint32_t base = 0;
    if (!span_readable(match + sizeof(pattern), sizeof(base))) {
        return 0;
    }

    std::memcpy(&base, reinterpret_cast<void const*>(match + sizeof(pattern)), sizeof(base));
    if (!span_readable(base, sizeof(std::uintptr_t) * kMaxIndex)) {
        return 0;
    }

    g_entity_array = base;
    return g_entity_array;
}

bool live_position(DWORD index, Position& out) {
    std::uintptr_t const base = entity_array();
    if (base == 0 || index == 0 || index >= kMaxIndex) {
        return false;
    }

    std::uintptr_t const slot = base + static_cast<std::uintptr_t>(index) * sizeof(std::uintptr_t);
    if (!span_readable(slot, sizeof(std::uintptr_t))) {
        return false;
    }

    std::uintptr_t entity = 0;
    std::memcpy(&entity, reinterpret_cast<void const*>(slot), sizeof(entity));
    if (entity == 0) {
        return false;
    }

    float coords[3] {};
    bool have = false;

    if (span_readable(entity + kEntityDisplayPtr, sizeof(std::uintptr_t))) {
        std::uintptr_t display = 0;
        std::memcpy(&display, reinterpret_cast<void const*>(entity + kEntityDisplayPtr), sizeof(display));
        if (display != 0 && span_readable(display + kDisplayNameplateBase, sizeof(float) * 3)) {
            std::memcpy(coords, reinterpret_cast<void const*>(display + kDisplayNameplateBase),
                sizeof(coords));
            have = true;
        }
    }

    if (!have) {
        if (!span_readable(entity + kEntityDisplayPos, sizeof(float) * 3)) {
            return false;
        }

        std::memcpy(coords, reinterpret_cast<void const*>(entity + kEntityDisplayPos), sizeof(coords));
    }

    if (!std::isfinite(coords[0]) || !std::isfinite(coords[1]) || !std::isfinite(coords[2])
        || std::fabs(coords[0]) > 10000.0f || std::fabs(coords[1]) > 10000.0f
        || std::fabs(coords[2]) > 10000.0f) {
        return false;
    }

    out.east = coords[0];
    out.height = coords[1];
    out.north = coords[2];
    return true;
}

// Only reached when the entity array cannot supply a live position. Lua's
// coordinates are the server-side value (~2 Hz), so they are extrapolated by
// velocity between samples rather than used raw.
Position smooth_ring_position(DWORD index, Position const& target) {
    if (index == 0 || index >= kMaxIndex) {
        return target;
    }

    MotionState& state = g_motion[index];
    DWORD const now = GetTickCount();

    float dt = 0.016f;
    if (state.initialized && state.last_ms != 0 && now > state.last_ms) {
        dt = static_cast<float>(now - state.last_ms) * 0.001f;
        dt = std::fmin(std::fmax(dt, 0.001f), 0.05f);
    }

    if (!state.initialized) {
        state.pos = target;
        state.initialized = true;
        state.last_ms = now;
        return state.pos;
    }

    if (g_draw_samples_fresh) {
        if (dt > 0.0f) {
            state.velocity.east = (target.east - state.pos.east) / dt;
            state.velocity.north = (target.north - state.pos.north) / dt;
            state.velocity.height = (target.height - state.pos.height) / dt;
        }

        state.pos = target;
        state.last_ms = now;
        return state.pos;
    }

    Position predicted = state.pos;
    predicted.east += state.velocity.east * dt;
    predicted.north += state.velocity.north * dt;
    predicted.height += state.velocity.height * dt;

    state.pos.east = predicted.east + (target.east - predicted.east) * (1.0f - kSmoothBlend);
    state.pos.north = predicted.north + (target.north - predicted.north) * (1.0f - kSmoothBlend);
    state.pos.height = predicted.height + (target.height - predicted.height) * (1.0f - kSmoothBlend);
    state.last_ms = now;
    return state.pos;
}

void refresh_player_occluder(const D3DVIEWPORT8& viewport) {
    g_player_occluder_valid = false;
    Position root {};
    DWORD index = g_player_index;
    if (index == 0 && g_chant.active) {
        index = g_chant.index;
    }
    if (index != 0 && live_position(index, root)) {
        // live entity pose
    } else if (g_player_has_pos) {
        root = g_player_pos;
    } else {
        return;
    }

    const Position feet {root.east, root.north, root.height};
    const Position plus {root.east, root.north, root.height + kPlayerBodyHeight};
    const Position minus {root.east, root.north, root.height - kPlayerBodyHeight};
    const Position side {root.east + kPlayerOccludeRadius, root.north, root.height};

    float foot_x = 0.0f;
    float foot_y = 0.0f;
    float foot_rhw = 1.0f;
    if (!world_to_screen(feet, viewport, foot_x, foot_y, foot_rhw) || foot_rhw <= 0.0f) {
        return;
    }

    // World "up" can land toward the camera in FFXI's view, which put the old
    // capsule in front of the character (a hole by the near edge). Measure the
    // body span, then always plant the capsule upward on screen from the feet.
    float plus_x = 0.0f;
    float plus_y = 0.0f;
    float plus_rhw = 1.0f;
    float minus_x = 0.0f;
    float minus_y = 0.0f;
    float minus_rhw = 1.0f;
    const bool plus_ok = world_to_screen(plus, viewport, plus_x, plus_y, plus_rhw)
        && plus_rhw > 0.0f;
    const bool minus_ok = world_to_screen(minus, viewport, minus_x, minus_y, minus_rhw)
        && minus_rhw > 0.0f;

    float span = 72.0f;
    if (plus_ok && minus_ok) {
        span = std::hypot(plus_x - minus_x, plus_y - minus_y);
    } else if (plus_ok) {
        span = std::hypot(plus_x - foot_x, plus_y - foot_y);
    } else if (minus_ok) {
        span = std::hypot(minus_x - foot_x, minus_y - foot_y);
    }
    span = std::fmax(36.0f, std::fmin(span, 220.0f));

    g_player_foot_x = foot_x;
    g_player_foot_y = foot_y;
    g_player_head_x = foot_x;
    g_player_head_y = foot_y - span;

    g_player_half_width = 18.0f;
    float side_x = 0.0f;
    float side_y = 0.0f;
    float side_rhw = 1.0f;
    if (world_to_screen(side, viewport, side_x, side_y, side_rhw) && side_rhw > 0.0f) {
        g_player_half_width = std::hypot(side_x - foot_x, side_y - foot_y);
    }
    g_player_half_width = std::fmax(10.0f, std::fmin(g_player_half_width, 90.0f));
    g_player_occluder_valid = true;
}

void lock_rings() {
    if (g_ring_lock_ready) {
        EnterCriticalSection(&g_ring_lock);
    }
}

void unlock_rings() {
    if (g_ring_lock_ready) {
        LeaveCriticalSection(&g_ring_lock);
    }
}

struct ArialGlyph {
    int w = 0;
    int h = 0;
    unsigned char pixels[96 * 96] {};
};

ArialGlyph g_arial_glyphs[4] {};
bool g_arial_ready = false;

// Bake Arial Bold N/E/S/W once via GDI, then tint the coverage with the ring colour.
bool rasterize_arial_bold() {
    if (g_arial_ready) {
        return true;
    }

    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) {
        return false;
    }

    HFONT font = CreateFontW(-56, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
    if (!font) {
        DeleteDC(hdc);
        return false;
    }

    HGDIOBJ previous = SelectObject(hdc, font);
    MAT2 mat {};
    mat.eM11.value = 1;
    mat.eM22.value = 1;
    wchar_t const chars[4] = { L'N', L'E', L'S', L'W' };
    unsigned char buffer[96 * 96];
    int baked = 0;

    for (int i = 0; i < 4; ++i) {
        GLYPHMETRICS gm {};
        DWORD needed = GetGlyphOutlineW(hdc, chars[i], GGO_GRAY8_BITMAP, &gm, 0, nullptr, &mat);
        if (needed == GDI_ERROR || needed == 0 || needed > sizeof(buffer)) {
            continue;
        }
        if (GetGlyphOutlineW(hdc, chars[i], GGO_GRAY8_BITMAP, &gm, needed, buffer, &mat) == GDI_ERROR) {
            continue;
        }

        int const w = static_cast<int>(gm.gmBlackBoxX);
        int const h = static_cast<int>(gm.gmBlackBoxY);
        if (w <= 0 || h <= 0) {
            continue;
        }
        int const stride = static_cast<int>(needed / static_cast<DWORD>(h));
        ArialGlyph& glyph = g_arial_glyphs[i];
        glyph.w = w < 96 ? w : 96;
        glyph.h = h < 96 ? h : 96;
        std::memset(glyph.pixels, 0, sizeof(glyph.pixels));
        for (int y = 0; y < glyph.h; ++y) {
            int copy = glyph.w;
            if (copy > stride) {
                copy = stride;
            }
            std::memcpy(glyph.pixels + y * 96, buffer + y * stride, static_cast<std::size_t>(copy));
        }
        ++baked;
    }

    SelectObject(hdc, previous);
    DeleteObject(font);
    DeleteDC(hdc);
    g_arial_ready = baked == 4;
    return g_arial_ready;
}

// Arial Bold N/E/S/W laid flat on the ground with the chant ring. Each letter
// sits on the outer glow; the top of the glyph points outward so they read as
// compass marks. Tinted with the live ring colour at 80% opacity. Colorblind
// mode adds a dark outline and a brighter fill so the letters stay readable.
void emit_chant_glyph(ArialGlyph const& glyph, const Position& origin, float h,
    float cell, float re, float rn, float oe, float on,
    const D3DVIEWPORT8& viewport, DWORD fill, int min_cov) {
    if (glyph.w <= 0 || glyph.h <= 0) {
        return;
    }

    const float half_w = static_cast<float>(glyph.w) * cell * 0.5f;
    const float half_h = static_cast<float>(glyph.h) * cell * 0.5f;

    for (int r = 0; r < glyph.h; ++r) {
        for (int c = 0; c < glyph.w; ++c) {
            const unsigned char cov = glyph.pixels[r * 96 + c];
            if (cov < min_cov) {
                continue;
            }
            const DWORD pix = scale_alpha(fill, static_cast<float>(cov) / 64.0f);
            const float u0 = -half_w + static_cast<float>(c) * cell;
            const float u1 = u0 + cell;
            const float v0 = half_h - static_cast<float>(r) * cell;
            const float v1 = v0 - cell;

            const Position a {
                origin.east + re * u0 + oe * v0,
                origin.north + rn * u0 + on * v0, h};
            const Position b {
                origin.east + re * u1 + oe * v0,
                origin.north + rn * u1 + on * v0, h};
            const Position cpos {
                origin.east + re * u1 + oe * v1,
                origin.north + rn * u1 + on * v1, h};
            const Position d {
                origin.east + re * u0 + oe * v1,
                origin.north + rn * u0 + on * v1, h};
            emit_quad(a, b, cpos, d, viewport, pix);
        }
    }
}

void draw_chant_compass(const Position& centre, float radius,
    const D3DVIEWPORT8& viewport, DWORD color) {
    if ((color & 0xFF000000u) == 0 || !rasterize_arial_bold()) {
        return;
    }

    const float dist = radius * 1.18f;
    const float h = centre.height - kGroundClearance + 0.02f;
    const Position points[4] = {
        {centre.east, centre.north + dist, h},
        {centre.east + dist, centre.north, h},
        {centre.east, centre.north - dist, h},
        {centre.east - dist, centre.north, h},
    };
    const float out_e[4] = { 0.0f,  1.0f,  0.0f, -1.0f };
    const float out_n[4] = { 1.0f,  0.0f, -1.0f,  0.0f };
    const float right_e[4] = { 1.0f,  0.0f, -1.0f,  0.0f };
    const float right_n[4] = { 0.0f, -1.0f,  0.0f,  1.0f };

    const DWORD fill = g_colorblind ? boost_chant_letter(color) : scale_alpha(color, 0.80f);
    constexpr DWORD kOutline = 0xF2000000;

    for (int i = 0; i < 4; ++i) {
        CompassMark mark {};
        mark.ok = world_to_screen(points[i], viewport, mark.x, mark.y, mark.rhw, mark.z)
            && mark.rhw > 0.0f;
        if (!mark.ok) {
            mark.x = -1.0f;
            mark.y = -1.0f;
            g_compass[i] = mark;
            continue;
        }
        g_compass[i] = mark;

        ArialGlyph const& glyph = g_arial_glyphs[i];
        if (glyph.w <= 0 || glyph.h <= 0) {
            continue;
        }

        const float world_h = dist * 0.195f;
        const float cell = world_h / static_cast<float>(glyph.h);
        const Position origin = points[i];

        if (g_colorblind) {
            const float halo = cell * 1.45f;
            const float ox[4] = { -halo, halo, 0.0f, 0.0f };
            const float oy[4] = { 0.0f, 0.0f, -halo, halo };
            for (int k = 0; k < 4; ++k) {
                const Position rim {
                    origin.east + right_e[i] * ox[k] + out_e[i] * oy[k],
                    origin.north + right_n[i] * ox[k] + out_n[i] * oy[k],
                    h};
                emit_chant_glyph(glyph, rim, h, cell, right_e[i], right_n[i],
                    out_e[i], out_n[i], viewport, kOutline, 4);
            }
        }

        emit_chant_glyph(glyph, origin, h, cell, right_e[i], right_n[i],
            out_e[i], out_n[i], viewport, fill, 8);
    }
}

void draw_range_guide(const Position& centre, float radius, const D3DVIEWPORT8& viewport, DWORD color) {
    if (radius <= 0.0f) {
        return;
    }
    if (color == 0) {
        color = 0xFFFFFCD2;
    }
    color = retint_range(color);
    // +25% opacity from the previous pass (0.10 / 0.238).
    draw_ground_ring(centre, radius, 0.16f, viewport, scale_alpha(color, 0.125f));
    draw_ground_ring(centre, radius, 0.055f, viewport, scale_alpha(mix_rgb(color, 0xFFFFFFFFu, 0.25f), 0.298f));
}

void draw_all_rings() {
    if (!d3d_device_) {
        return;
    }

    Ring rings[kMaxRings] {};
    Ring chant {};
    Ring range_rings[kMaxRangeRings] {};
    int count = 0;
    int range_count = 0;
    lock_rings();
    count = g_ring_count;
    if (count > kMaxRings) {
        count = kMaxRings;
    }
    if (count > 0) {
        std::memcpy(rings, g_rings, static_cast<std::size_t>(count) * sizeof(Ring));
    }
    chant = g_chant;
    range_count = g_range_count;
    if (range_count > kMaxRangeRings) {
        range_count = kMaxRangeRings;
    }
    if (range_count > 0) {
        std::memcpy(range_rings, g_range, static_cast<std::size_t>(range_count) * sizeof(Ring));
    }
    g_draw_samples_fresh = g_samples_fresh;
    g_samples_fresh = false;
    unlock_rings();

    if (count <= 0 && !chant.active && range_count <= 0) {
        g_compass[0].ok = g_compass[1].ok = g_compass[2].ok = g_compass[3].ok = false;
        return;
    }

    D3DVIEWPORT8 viewport {};
    if (dev_GetViewport(&viewport) < 0 || viewport.Width == 0 || viewport.Height == 0) {
        return;
    }

    if (!refresh_projection_matrices()) {
        return;
    }

    update_draw_quality();

    if (!begin_draw_state()) {
        return;
    }

    batch_vertex_count_ = 0;
    const float phase = static_cast<float>(GetTickCount() % kPulsePeriodMs)
        / static_cast<float>(kPulsePeriodMs);
    const float pulse = 1.0f - kPulseDepth * 0.5f * (1.0f - std::cos(phase * kTwoPi));

    for (int i = 0; i < count && i < kMaxRings; ++i) {
        Ring const& ring = rings[i];
        if (!ring.active || ring.radius <= 0.0f) {
            continue;
        }

        Position centre {};
        if (!live_position(ring.index, centre)) {
            centre = smooth_ring_position(ring.index, ring.centre);
        }

        const Position hub {centre.east, centre.north, centre.height - kGroundClearance};
        float hx = 0.0f;
        float hy = 0.0f;
        float hrhw = 1.0f;
        if (world_to_screen(hub, viewport, hx, hy, hrhw) && hrhw > 0.0f) {
            g_fade_hub = 1.0f / hrhw;
            g_fade_span = ring.radius;
        } else {
            g_fade_span = 0.0f;
        }

        if (g_draw_outer_glow) {
            draw_ground_ring(centre, ring.radius * 1.18f, ring.radius * 0.34f,
                viewport, scale_alpha(ring.color, kGlowAlpha * pulse));
        }
        draw_ground_ring(centre, ring.radius, ring.radius * 0.12f,
            viewport, scale_alpha(ring.color, 0.95f * pulse));
        if (g_colorblind) {
            draw_colorblind_markers(centre, ring.radius, ring.color, viewport);
        }
    }

    // Cardinal Chant and range rings skip the depth test so hills do not eat
    // the circle. Tag rings keep hardware Z so they tuck behind characters.
    // With Z off we restore rear-depth fade and a soft local-player cutout.
    flush_batch();
    dev_SetRenderState(D3DRS_ZENABLE, FALSE);
    g_soft_player_cutout = true;
    refresh_player_occluder(viewport);

    if (chant.active && chant.radius > 0.0f) {
        Position centre {};
        if (!live_position(chant.index, centre)) {
            centre = smooth_ring_position(chant.index, chant.centre);
        }

        const Position hub {centre.east, centre.north, centre.height - kGroundClearance};
        float hx = 0.0f;
        float hy = 0.0f;
        float hrhw = 1.0f;
        if (world_to_screen(hub, viewport, hx, hy, hrhw) && hrhw > 0.0f) {
            g_fade_hub = 1.0f / hrhw;
            g_fade_span = chant.radius;
        } else {
            g_fade_span = 0.0f;
        }

        if (g_draw_outer_glow) {
            draw_ground_ring(centre, chant.radius * 1.18f, chant.radius * 0.34f,
                viewport, scale_alpha(chant.color, kGlowAlpha * pulse));
        }
        draw_ground_ring(centre, chant.radius, chant.radius * 0.12f,
            viewport, scale_alpha(chant.color, 0.95f * pulse));

        draw_chant_compass(centre, chant.radius, viewport,
            g_colorblind ? chant.color : scale_alpha(chant.color, 0.95f * pulse));
    } else {
        g_compass[0].ok = g_compass[1].ok = g_compass[2].ok = g_compass[3].ok = false;
    }

    for (int i = 0; i < range_count; ++i) {
        Ring const& ring = range_rings[i];
        if (!ring.active || ring.radius <= 0.0f) {
            continue;
        }
        Position centre {};
        if (!live_position(ring.index, centre)) {
            centre = smooth_ring_position(ring.index, ring.centre);
        }
        const Position hub {centre.east, centre.north, centre.height - kGroundClearance};
        float hx = 0.0f;
        float hy = 0.0f;
        float hrhw = 1.0f;
        if (world_to_screen(hub, viewport, hx, hy, hrhw) && hrhw > 0.0f) {
            g_fade_hub = 1.0f / hrhw;
            g_fade_span = ring.radius;
        } else {
            g_fade_span = 0.0f;
        }
        g_fade_floor = kRangeRearFade;
        draw_range_guide(centre, ring.radius, viewport, ring.color);
        g_fade_floor = kRearFade;
    }
    g_soft_player_cutout = false;
    flush_batch();
    end_draw_state();
    ++g_draws;
}

// FFXI does not keep the D3D8 device anywhere reachable by signature, but the
// renderer holds d3d8 resources, and every IDirect3DResource8 can hand back its
// creator via GetDevice at vtable slot 3. Scanning for a resource and asking it
// is far more reliable than trying to recognise the device by its vtable.
bool looks_like_d3d_vtable(std::uintptr_t vtable, std::uintptr_t d3d_base, std::size_t d3d_size) {
    if (vtable < d3d_base || vtable + 16 > d3d_base + d3d_size) {
        return false;
    }
    if (!span_readable(vtable, sizeof(std::uintptr_t) * 4)) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        std::uintptr_t slot = 0;
        std::memcpy(&slot, reinterpret_cast<void const*>(vtable + i * sizeof(std::uintptr_t)),
            sizeof(slot));
        if (slot < d3d_base || slot >= d3d_base + d3d_size) {
            return false;
        }
    }
    return true;
}

// A wrapper chain can leave the resources in d3d8.dll while the device object
// itself lives in another module, so same-module is treated as confirmation
// rather than a requirement. Anything else has to look structurally like a
// device: the four slots we actually call must resolve to real code.
bool plausible_device_vtable(std::uintptr_t vtable) {
    if (!span_readable(vtable, sizeof(std::uintptr_t) * kDeviceVtableEntries)) {
        return false;
    }

    static int const probes[] = { 2, 35, 41, 72 };
    for (int slot : probes) {
        std::uintptr_t entry = 0;
        std::memcpy(&entry,
            reinterpret_cast<void const*>(vtable + static_cast<std::uintptr_t>(slot) * sizeof(std::uintptr_t)),
            sizeof(entry));
        if (entry == 0) {
            return false;
        }

        MEMORY_BASIC_INFORMATION region {};
        if (!VirtualQuery(reinterpret_cast<void const*>(entry), &region, sizeof(region))
            || region.State != MEM_COMMIT
            || !page_executable(region.Protect)) {
            return false;
        }
    }

    return true;
}

bool try_accept_d3d_resource(std::uintptr_t resource, std::uintptr_t d3d_base, std::size_t d3d_size) {
    if (resource == 0 || !span_readable(resource, sizeof(std::uintptr_t))) {
        return false;
    }

    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, reinterpret_cast<void const*>(resource), sizeof(vtable));
    if (!looks_like_d3d_vtable(vtable, d3d_base, d3d_size)) {
        return false;
    }

    auto get_device = reinterpret_cast<fn_get_device>(vtable_slot(resource, 3));
    if (!get_device) {
        return false;
    }

    void* device = nullptr;
    if (get_device(reinterpret_cast<void*>(resource), &device) < 0 || !device) {
        return false;
    }

    std::uintptr_t const address = reinterpret_cast<std::uintptr_t>(device);
    if (!span_readable(address, sizeof(std::uintptr_t))) {
        // get_device AddRef'd the object; release before bailing
        auto release = reinterpret_cast<fn_release>(vtable_slot(address, 2));
        if (release) {
            release(device);
        }
        return false;
    }

    std::uintptr_t device_vtable = 0;
    std::memcpy(&device_vtable, device, sizeof(device_vtable));

    bool const same_module = device_vtable >= d3d_base
        && device_vtable < d3d_base + d3d_size;
    if (!same_module && !plausible_device_vtable(device_vtable)) {
        auto release = reinterpret_cast<fn_release>(vtable_slot(address, 2));
        if (release) {
            release(device);
        }
        return false;
    }

    g_device = address;
    d3d_device_ = device;
    g_discovery_done = true;

    auto release = reinterpret_cast<fn_release>(vtable_slot(address, 2));
    if (release) {
        release(device);
    }

    std::snprintf(g_status, sizeof(g_status),
        same_module ? "running" : "running (wrapped device)");
    return true;
}

bool scan_module_for_device(char const* name) {
    std::uintptr_t d3d_base = 0;
    std::size_t d3d_size = 0;
    if (!module_range("d3d8.dll", d3d_base, d3d_size)) {
        return false;
    }

    std::uintptr_t base = 0;
    std::size_t size = 0;
    if (!module_range(name, base, size) || base == 0) {
        return false;
    }

    std::uintptr_t address = base;
    std::uintptr_t const end = base + size;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region {};
        if (!VirtualQuery(reinterpret_cast<void const*>(address), &region, sizeof(region))) {
            break;
        }

        std::uintptr_t const region_end = reinterpret_cast<std::uintptr_t>(region.BaseAddress)
            + region.RegionSize;
        if (region.State != MEM_COMMIT || !page_readable(region.Protect)
            || (region.Protect & (PAGE_GUARD | PAGE_EXECUTE | PAGE_EXECUTE_READ
                | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            address = region_end;
            continue;
        }

        std::uintptr_t const scan_end = region_end < end ? region_end : end;
        for (std::uintptr_t slot = address; slot + 4 <= scan_end; slot += 4) {
            std::uintptr_t resource = 0;
            std::memcpy(&resource, reinterpret_cast<void const*>(slot), sizeof(resource));
            if (try_accept_d3d_resource(resource, d3d_base, d3d_size)) {
                return true;
            }
        }
        address = region_end;
    }

    return false;
}

// FFXI does not keep the D3D8 device anywhere reachable by signature, but the
// renderer holds d3d8 resources, and every IDirect3DResource8 can hand back its
// creator via GetDevice at vtable slot 3. Scanning for a resource and asking it
// is far more reliable than trying to recognise the device by its vtable.
void acquire_device(std::uintptr_t renderer) {
    if (d3d_device_ || renderer == 0 || g_device_attempts >= kMaxDeviceAttempts) {
        return;
    }

    if (g_device_attempts != 0 && (g_frames % kDeviceRetryFrames) != 0) {
        return;
    }
    ++g_device_attempts;

    std::uintptr_t d3d_base = 0;
    std::size_t d3d_size = 0;
    if (!module_range("d3d8.dll", d3d_base, d3d_size)) {
        std::snprintf(g_status, sizeof(g_status), "waiting for d3d8");
        return;
    }

    // An unreadable slot is skipped rather than ending the scan; the renderer
    // has gaps and the device resource can sit past them.
    for (std::size_t offset = 0; offset + 4 <= kRendererScanWindow; offset += 4) {
        std::uintptr_t const slot = renderer + offset;
        if (!span_readable(slot, sizeof(std::uintptr_t))) {
            continue;
        }

        std::uintptr_t resource = 0;
        std::memcpy(&resource, reinterpret_cast<void const*>(slot), sizeof(resource));
        if (try_accept_d3d_resource(resource, d3d_base, d3d_size)) {
            return;
        }
    }

    std::snprintf(g_status, sizeof(g_status), "device not found in renderer (try %lu)",
        g_device_attempts);
}

bool page_executable(DWORD protect) {
    switch (protect & 0xFF) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

// Called from inside draw_scene, depth buffer still bound.
void SCENEHOOK_ALIGN_STACK __cdecl scene_draw(void* user, void* renderer, void* device) {
    (void)user;
    (void)device;

    g_renderer = reinterpret_cast<std::uintptr_t>(renderer);
    ++g_frames;
    acquire_device(g_renderer);
    draw_all_rings();
}

bool install_hook() {
    if (!g_bus) {
        g_bus = scenehook_attach();
    }

    if (!g_bus) {
        std::snprintf(g_status, sizeof(g_status),
            "scene hook unavailable (another addon may be built against a different ABI)");
        return false;
    }

    if (!scenehook_ensure_hook(g_bus)) {
        std::snprintf(g_status, sizeof(g_status), "%s", g_bus->status);
        return false;
    }

    if (g_bus_slot < 0) {
        g_bus_slot = scenehook_register(g_bus, &scene_draw, nullptr);
        if (g_bus_slot < 0) {
            std::snprintf(g_status, sizeof(g_status), "scene hook is full");
            return false;
        }
    }

    scenehook_set_enabled(g_bus, g_bus_slot, true);
    g_hooked = true;
    std::snprintf(g_status, sizeof(g_status), "running");
    return true;
}

// The patch is shared and is never removed. The slot is released in DllMain.
bool remove_hook() {
    lock_rings();
    g_ring_count = 0;
    unlock_rings();

    g_hooked = false;
    g_device_attempts = 0;

    scenehook_set_enabled(g_bus, g_bus_slot, false);
    std::snprintf(g_status, sizeof(g_status), "stopped");
    return true;
}

int __cdecl lua_start(lua_State* L) {
    install_hook();
    g_lua.pushstring(L, g_status);
    return 1;
}

int __cdecl lua_stop(lua_State* L) {
    remove_hook();
    g_lua.pushstring(L, g_status);
    return 1;
}

int __cdecl lua_status(lua_State* L) {
    char bus[224] {};
    scenehook_describe(g_bus, g_bus_slot, bus, sizeof(bus));

    char report[512] {};
    std::snprintf(report, sizeof(report),
        "n46 drawing=%s frames=%lu draws=%lu device=%08lX rings=%d | %s | %s",
        g_hooked ? "yes" : "no", g_frames, g_draws,
        static_cast<unsigned long>(g_device), g_ring_count, g_status, bus);
    g_lua.pushstring(L, report);
    return 1;
}

int __cdecl lua_clear(lua_State* L) {
    g_staging_count = 0;
    for (int i = 0; i < kMaxRings; ++i) {
        g_staging[i].active = false;
    }

    (void)L;
    return 0;
}

int __cdecl lua_commit(lua_State* L) {
    lock_rings();
    std::memcpy(g_rings, g_staging, sizeof(g_rings));
    g_ring_count = g_staging_count;
    g_samples_fresh = true;
    unlock_rings();

    (void)L;
    return 0;
}

int __cdecl lua_player(lua_State* L) {
    if (g_lua.gettop(L) < 4) {
        g_player_index = 0;
        g_player_has_pos = false;
        return 0;
    }

    g_player_index = static_cast<DWORD>(g_lua.tonumber(L, 1));
    g_player_pos.east = static_cast<float>(g_lua.tonumber(L, 2));
    g_player_pos.north = static_cast<float>(g_lua.tonumber(L, 3));
    g_player_pos.height = static_cast<float>(g_lua.tonumber(L, 4));
    g_player_has_pos = g_player_index != 0;
    return 0;
}

int __cdecl lua_colorblind(lua_State* L) {
    if (g_lua.gettop(L) >= 1) {
        g_colorblind = g_lua.tonumber(L, 1) != 0.0;
    }
    return 0;
}

int __cdecl lua_chant(lua_State* L) {
    Ring ring {};
    if (g_lua.gettop(L) >= 6) {
        ring.index = static_cast<DWORD>(g_lua.tonumber(L, 1));
        ring.centre.east = static_cast<float>(g_lua.tonumber(L, 2));
        ring.centre.north = static_cast<float>(g_lua.tonumber(L, 3));
        ring.centre.height = static_cast<float>(g_lua.tonumber(L, 4));
        ring.radius = static_cast<float>(g_lua.tonumber(L, 5));
        ring.color = static_cast<DWORD>(g_lua.tonumber(L, 6));
        ring.active = ring.index != 0 && ring.radius > 0.0f;
    }

    lock_rings();
    g_chant = ring;
    unlock_rings();
    return 0;
}

int __cdecl lua_range_clear(lua_State* L) {
    g_range_staging_count = 0;
    for (int i = 0; i < kMaxRangeRings; ++i) {
        g_range_staging[i].active = false;
    }
    (void)L;
    return 0;
}

int __cdecl lua_range_add(lua_State* L) {
    if (g_lua.gettop(L) < 5) {
        return 0;
    }
    int const slot = g_range_staging_count;
    if (slot >= kMaxRangeRings) {
        return 0;
    }
    Ring ring {};
    ring.index = static_cast<DWORD>(g_lua.tonumber(L, 1));
    ring.centre.east = static_cast<float>(g_lua.tonumber(L, 2));
    ring.centre.north = static_cast<float>(g_lua.tonumber(L, 3));
    ring.centre.height = static_cast<float>(g_lua.tonumber(L, 4));
    ring.radius = static_cast<float>(g_lua.tonumber(L, 5));
    ring.color = g_lua.gettop(L) >= 6
        ? static_cast<DWORD>(g_lua.tonumber(L, 6))
        : 0xFFFFFCD2;
    ring.active = ring.index != 0 && ring.radius > 0.0f;
    g_range_staging[slot] = ring;
    g_range_staging_count = slot + 1;
    return 0;
}

int __cdecl lua_range_commit(lua_State* L) {
    lock_rings();
    std::memcpy(g_range, g_range_staging, sizeof(g_range));
    g_range_count = g_range_staging_count;
    g_samples_fresh = true;
    unlock_rings();
    (void)L;
    return 0;
}

int __cdecl lua_compass(lua_State* L) {
    if (!g_lua.pushnumber) {
        return 0;
    }
    for (int i = 0; i < 4; ++i) {
        if (g_compass[i].ok) {
            g_lua.pushnumber(L, g_compass[i].x);
            g_lua.pushnumber(L, g_compass[i].y);
        } else {
            g_lua.pushnumber(L, -1.0);
            g_lua.pushnumber(L, -1.0);
        }
    }
    return 8;
}

int __cdecl lua_add(lua_State* L) {
    if (g_lua.gettop(L) < 6) {
        return 0;
    }

    int const slot = g_staging_count;
    if (slot >= kMaxRings) {
        return 0;
    }

    Ring ring {};
    ring.index = static_cast<DWORD>(g_lua.tonumber(L, 1));
    ring.centre.east = static_cast<float>(g_lua.tonumber(L, 2));
    ring.centre.north = static_cast<float>(g_lua.tonumber(L, 3));
    ring.centre.height = static_cast<float>(g_lua.tonumber(L, 4));
    ring.radius = static_cast<float>(g_lua.tonumber(L, 5));
    ring.color = static_cast<DWORD>(g_lua.tonumber(L, 6));
    ring.active = true;

    g_staging[slot] = ring;
    g_staging_count = slot + 1;
    return 0;
}

bool bind_lua() {
    if (g_lua.ready()) {
        return true;
    }

    static char const* const hosts[] = {"LuaCore.dll", "lua51.dll", "lua5.1.dll"};
    for (char const* host : hosts) {
        HMODULE module = GetModuleHandleA(host);
        if (!module) {
            continue;
        }

        g_lua.createtable = reinterpret_cast<fn_createtable>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_createtable")));
        g_lua.pushcclosure = reinterpret_cast<fn_pushcclosure>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_pushcclosure")));
        g_lua.setfield = reinterpret_cast<fn_setfield>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_setfield")));
        g_lua.pushvalue = reinterpret_cast<fn_pushvalue>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_pushvalue")));
        g_lua.pushstring = reinterpret_cast<fn_pushstring>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_pushstring")));
        g_lua.gettop = reinterpret_cast<fn_gettop>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_gettop")));
        g_lua.tonumber = reinterpret_cast<fn_tonumber>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_tonumber")));
        g_lua.pushnumber = reinterpret_cast<fn_pushnumber>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_pushnumber")));

        if (g_lua.ready()) {
            return true;
        }
    }

    return false;
}

}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings(lua_State* L) {
    if (!bind_lua()) {
        return 0;
    }

    build_tables();

    lock_rings();
    g_ring_count = 0;
    unlock_rings();

    g_lua.createtable(L, 0, 14);

    g_lua.pushstring(L, "2.0.6");
    g_lua.setfield(L, -2, "native");

    g_lua.pushcclosure(L, lua_start, 0);
    g_lua.setfield(L, -2, "start");

    g_lua.pushcclosure(L, lua_stop, 0);
    g_lua.setfield(L, -2, "stop");

    g_lua.pushcclosure(L, lua_status, 0);
    g_lua.setfield(L, -2, "status");

    g_lua.pushcclosure(L, lua_clear, 0);
    g_lua.setfield(L, -2, "clear");

    g_lua.pushcclosure(L, lua_commit, 0);
    g_lua.setfield(L, -2, "commit");

    g_lua.pushcclosure(L, lua_add, 0);
    g_lua.setfield(L, -2, "add");

    g_lua.pushcclosure(L, lua_player, 0);
    g_lua.setfield(L, -2, "player");

    g_lua.pushcclosure(L, lua_colorblind, 0);
    g_lua.setfield(L, -2, "colorblind");

    g_lua.pushcclosure(L, lua_chant, 0);
    g_lua.setfield(L, -2, "chant");

    g_lua.pushcclosure(L, lua_range_clear, 0);
    g_lua.setfield(L, -2, "range_clear");

    g_lua.pushcclosure(L, lua_range_add, 0);
    g_lua.setfield(L, -2, "range_add");

    g_lua.pushcclosure(L, lua_range_commit, 0);
    g_lua.setfield(L, -2, "range_commit");

    g_lua.pushcclosure(L, lua_compass, 0);
    g_lua.setfield(L, -2, "compass");

    g_lua.pushvalue(L, -1);
    g_lua.setfield(L, kGlobalsIndex, "_GEORings");
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl luaopen_geohud_rings(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen_geohud_rings2(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings23(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings24(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings25(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings26(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings27(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings28(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings29(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings30(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings31(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings32(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings33(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings34(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings35(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings36(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings37(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings38(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings39(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings40(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings41(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings42(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings43(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings44(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings45(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings46(lua_State* L) {
    return luaopen__GEORings(L);
}

extern "C" __declspec(dllexport) int __cdecl luaopen__GEORings_new(lua_State* L) {
    return luaopen__GEORings(L);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        InitializeCriticalSection(&g_ring_lock);
        g_ring_lock_ready = true;
    } else if (reason == DLL_PROCESS_DETACH) {
        // Runs however the addon goes away, and before the image is
        // unmapped. Hands the frame to a surviving client if we own it.
        // may_wait is false: the loader lock is held here.
        scenehook_unregister(g_bus, g_bus_slot, false);
        g_bus_slot = -1;
        if (g_ring_lock_ready) {
            g_ring_lock_ready = false;
            DeleteCriticalSection(&g_ring_lock);
        }
    }

    return TRUE;
}
