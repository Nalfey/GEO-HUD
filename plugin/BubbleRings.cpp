#include "PluginInterface.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {
HMODULE g_module = nullptr;

void module_log_path(char* output, std::size_t output_size) {
    if (!output || output_size == 0) {
        return;
    }

    output[0] = '\0';

    char module_path[MAX_PATH] {};
    if (!g_module || !GetModuleFileNameA(g_module, module_path, sizeof(module_path))) {
        return;
    }

    char* slash = std::strrchr(module_path, '\\');
    if (!slash) {
        return;
    }

    *slash = '\0';
    std::snprintf(output, output_size, "%s\\settings\\BubbleRings\\native.log", module_path);
}

void append_module_log(const char* message) {
    char path[MAX_PATH] {};
    module_log_path(path, sizeof(path));
    if (path[0] == '\0') {
        return;
    }

    FILE* file = std::fopen(path, "ab");
    if (!file) {
        return;
    }

    std::time_t now = std::time(nullptr);
    std::tm* local = std::localtime(&now);
    if (local) {
        char stamp[32] {};
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", local);
        std::fprintf(file, "%s %s\n", stamp, message);
    } else {
        std::fprintf(file, "%s\n", message);
    }

    std::fclose(file);
}
}

class BubbleRingsPlugin final : public PluginBase {
public:
    const char* __stdcall GetPluginAuthor() override {
        return "Nalfey";
    }

    const char* __stdcall GetPluginName() override {
        return "bubblerings";
    }

    void __stdcall Load(PluginManager* manager) override {
        plugin_manager_ = manager;
        initialize_geometry_tables();
        append_module_log("Load called");
        initialize_paths_from_module();
        append_log("loaded");
        probe_device("load");
    }

    void __stdcall Unload() override {
        append_log("unloaded");
        close_state_change_notification();
    }

    void __stdcall PostRender() override {
        projection_matrices_valid_ = false;

        draw_ring_overlay();
    }

private:
    struct Position {
        float east = 0.0f;
        float north = 0.0f;
        float height = 0.0f;
    };

    static constexpr std::uintptr_t kContextSlot = 0x1c8400;
    static constexpr std::uintptr_t kTableSlot = 0x24;
    static constexpr std::uintptr_t kActorSlot = 0x0a0;
    static constexpr std::uintptr_t kRootX = 0x678;
    static constexpr std::uintptr_t kRootZ = 0x67C;
    static constexpr std::uintptr_t kRootY = 0x680;
    static constexpr std::uintptr_t kSkeletonSlot = 0x6B8;
    static constexpr std::uintptr_t kSkeletonIndirect = 0x0C;
    static constexpr std::uintptr_t kBoneCount = 0x32;
    static constexpr std::uintptr_t kBoneTableBase = 0x30;
    static constexpr std::uintptr_t kBoneTablePad = 0x08;
    static constexpr std::uintptr_t kBoneStride = 0x1E;
    static constexpr std::uintptr_t kBoneSize = 0x1A;
    static constexpr std::uintptr_t kBoneOffset = 0x0E;
    static constexpr std::size_t kEntryProbe = 0x0a4;
    static constexpr std::size_t kActorProbe = 0x700;
    static constexpr DWORD kMaxIndex = 0x900;
    static constexpr std::uint16_t kMaxBones = 256;
    static constexpr float kWorldLimit = 10000.0f;
    static constexpr float kBoneLimit = 100.0f;

    struct DrawVertex {
        float x;
        float y;
        float z;
        float rhw;
        DWORD color;
    };

    struct Entity {
        DWORD index = 0;
        bool is_npc = false;
        float model_size = 0.0f;
        float model_scale = 1.0f;
        bool valid = false;
        bool active = false;
    };

    static constexpr int ring_slices_ = 48;
    static constexpr float kPlayerFootprint = 0.64f;
    static constexpr float kFadeSpan = 0.55f;
    static constexpr float kOpacity = 0.85f;
    static constexpr float kPlayerBodyHeight = 1.72f;
    static constexpr float kPlayerOccludeRadius = 0.42f;
    static constexpr unsigned kPulsePeriodMs = 1600u;
    static constexpr DWORD kActiveColour = 0xE63CFF6A;
    static constexpr DWORD kInactiveColour = 0xE6FF3C3C;
    static constexpr int kMaxTagged = 50;
    static constexpr int max_batch_vertices_ = 8192;

    void initialize_geometry_tables() {
        for (int i = 0; i <= ring_slices_; ++i) {
            const float angle = 6.28318530718f * static_cast<float>(i) / static_cast<float>(ring_slices_);
            ring_cos_[i] = std::cos(angle);
            ring_sin_[i] = std::sin(angle);
        }

    }

    void draw_ring_overlay() {
        read_state();

        if (tagged_count_ <= 0) {
            return;
        }

        if (!d3d_device_) {
            probe_device("draw");
        }

        if (!d3d_device_) {
            return;
        }

        D3DVIEWPORT8 viewport {};
        if (FAILED(d3d_device_->GetViewport(&viewport)) || viewport.Width == 0 || viewport.Height == 0) {
            return;
        }

        if (!refresh_projection_matrices()) {
            return;
        }

        std::uintptr_t mob_array = 0;
        if (!Locator::entity_table(mob_array)) {
            return;
        }

        const float phase = static_cast<float>(GetTickCount() % kPulsePeriodMs)
            / static_cast<float>(kPulsePeriodMs);
        const float pulse = 0.72f + 0.28f * std::sin(phase * 6.28318530718f);

        if (!begin_draw_state()) {
            return;
        }

        batch_vertex_count_ = 0;
        refresh_player_occluder(mob_array, viewport);

        for (int i = 0; i < tagged_count_; ++i) {
            if (tagged_[i].valid) {
                const DWORD color = tagged_[i].active ? kActiveColour : kInactiveColour;
                draw_entity_ring(mob_array, tagged_[i], viewport, color, pulse);
            }
        }

        flush_batch();
        end_draw_state();
    }

    void draw_entity_ring(std::uintptr_t mob_array, const Entity& entity,
        const D3DVIEWPORT8& viewport, DWORD color, float pulse) {
        std::uintptr_t actor = 0;
        Position root {};
        if (!Locator::actor_for(mob_array, entity.index, actor)
            || !Locator::ground_position(actor, root)) {
            return;
        }

        const float radius = entity_radius(entity);

        draw_ground_ring(root, radius * 1.18f, radius * 0.34f,
            viewport, scale_alpha(color, 0.30f * pulse * kOpacity));
        draw_ground_ring(root, radius, radius * 0.12f,
            viewport, scale_alpha(color, 0.95f * pulse * kOpacity));

    }

    void draw_ground_ring(const Position& centre, float radius,
        float band_width, const D3DVIEWPORT8& viewport, DWORD color) {
        if (radius <= 0.0f) {
            return;
        }

        const float inner_radius = std::fmax(radius - band_width * 0.5f, 0.01f);
        const float outer_radius = radius + band_width * 0.5f;

        // Negative z is up, so this floats the ring clear of the terrain.
        const Position hub {centre.east, centre.north, centre.height - 0.05f};

        float hub_x = 0.0f;
        float hub_y = 0.0f;
        float hub_rhw = 1.0f;
        if (!world_to_screen(hub, viewport, hub_x, hub_y, hub_rhw) || hub_rhw <= 0.0f) {
            return;
        }

        const float hub_distance = 1.0f / hub_rhw;

        float inner_x[ring_slices_ + 1] {};
        float inner_y[ring_slices_ + 1] {};
        float outer_x[ring_slices_ + 1] {};
        float outer_y[ring_slices_ + 1] {};
        DWORD inner_color[ring_slices_ + 1] {};
        DWORD outer_color[ring_slices_ + 1] {};
        bool resolved[ring_slices_ + 1] {};

        for (int i = 0; i <= ring_slices_; ++i) {
            const float cos_a = ring_cos_[i];
            const float sin_a = ring_sin_[i];

            float inner_rhw = 1.0f;
            float outer_rhw = 1.0f;

            const Position near_point {hub.east + inner_radius * cos_a,
                hub.north + inner_radius * sin_a, hub.height};
            const Position far_point {hub.east + outer_radius * cos_a,
                hub.north + outer_radius * sin_a, hub.height};

            const bool inner_ok = world_to_screen(near_point, viewport, inner_x[i], inner_y[i], inner_rhw);
            const bool outer_ok = world_to_screen(far_point, viewport, outer_x[i], outer_y[i], outer_rhw);

            resolved[i] = inner_ok && outer_ok && inner_rhw > 0.0f && outer_rhw > 0.0f;
            if (!resolved[i]) {
                continue;
            }

            const float inner_far = 1.0f - far_side_fade(1.0f / inner_rhw, hub_distance, radius);
            const float outer_far = 1.0f - far_side_fade(1.0f / outer_rhw, hub_distance, radius);
            inner_color[i] = scale_alpha(color,
                inner_far * (1.0f - player_cover(inner_x[i], inner_y[i])));
            outer_color[i] = scale_alpha(color,
                outer_far * (1.0f - player_cover(outer_x[i], outer_y[i])));
        }

        DrawVertex quad[6] {};
        for (int i = 0; i < ring_slices_; ++i) {
            if (!resolved[i] || !resolved[i + 1]) {
                continue;
            }

            if (((inner_color[i] | outer_color[i] | inner_color[i + 1] | outer_color[i + 1])
                & 0xFF000000u) == 0) {
                continue;
            }

            quad[0] = {inner_x[i], inner_y[i], 0.0f, 1.0f, inner_color[i]};
            quad[1] = {outer_x[i], outer_y[i], 0.0f, 1.0f, outer_color[i]};
            quad[2] = {inner_x[i + 1], inner_y[i + 1], 0.0f, 1.0f, inner_color[i + 1]};
            quad[3] = {inner_x[i + 1], inner_y[i + 1], 0.0f, 1.0f, inner_color[i + 1]};
            quad[4] = {outer_x[i], outer_y[i], 0.0f, 1.0f, outer_color[i]};
            quad[5] = {outer_x[i + 1], outer_y[i + 1], 0.0f, 1.0f, outer_color[i + 1]};

            append_batch(quad, 6);
        }
    }

    void refresh_player_occluder(std::uintptr_t mob_array, const D3DVIEWPORT8& viewport) {
        player_occluder_valid_ = false;
        if (player_index_ == 0) {
            return;
        }

        std::uintptr_t actor = 0;
        Position root {};
        if (!Locator::actor_for(mob_array, player_index_, actor)
            || !Locator::ground_position(actor, root)) {
            return;
        }

        // Negative z is up: raise the head, drop a little below the feet so the
        // ring at ground level is still covered by the silhouette.
        const Position feet {root.east, root.north, root.height + 0.12f};
        const Position head {root.east, root.north, root.height - kPlayerBodyHeight};
        const Position side {root.east + kPlayerOccludeRadius, root.north, root.height};

        float side_x = 0.0f;
        float side_y = 0.0f;
        float side_rhw = 1.0f;
        float feet_rhw = 1.0f;
        float head_rhw = 1.0f;
        if (!world_to_screen(feet, viewport, player_foot_x_, player_foot_y_, feet_rhw)
            || !world_to_screen(head, viewport, player_head_x_, player_head_y_, head_rhw)
            || feet_rhw <= 0.0f || head_rhw <= 0.0f) {
            return;
        }

        if (world_to_screen(side, viewport, side_x, side_y, side_rhw) && side_rhw > 0.0f) {
            player_half_width_ = std::hypot(side_x - player_foot_x_, side_y - player_foot_y_);
        } else {
            player_half_width_ = 18.0f;
        }

        player_half_width_ = std::fmax(10.0f, std::fmin(player_half_width_, 90.0f));
        player_occluder_valid_ = true;
    }

    float player_cover(float screen_x, float screen_y) const {
        if (!player_occluder_valid_) {
            return 0.0f;
        }

        const float distance = point_to_segment(screen_x, screen_y,
            player_foot_x_, player_foot_y_, player_head_x_, player_head_y_);
        const float inner = player_half_width_ * 0.70f;
        const float outer = player_half_width_ * 1.40f;
        return 1.0f - smoothstep(clamp01((distance - inner) / (outer - inner)));
    }

    static float point_to_segment(float px, float py, float ax, float ay, float bx, float by) {
        const float dx = bx - ax;
        const float dy = by - ay;
        const float length_sq = dx * dx + dy * dy;
        float t = 0.0f;
        if (length_sq > 0.0001f) {
            t = clamp01(((px - ax) * dx + (py - ay) * dy) / length_sq);
        }

        const float qx = ax + t * dx;
        const float qy = ay + t * dy;
        return std::hypot(px - qx, py - qy);
    }

    // The depth buffer is unusable from PostRender, so the far arc is faded by
    // distance behind the mob instead. Normalising by radius keeps it identical
    // on a hare and a dragon.
    float far_side_fade(float distance, float hub_distance, float radius) const {
        if (radius <= 0.0f) {
            return 0.0f;
        }

        const float span = std::fmax(radius * kFadeSpan, 0.01f);
        return smoothstep(clamp01((distance - hub_distance) / span));
    }

    float entity_radius(const Entity& entity) const {
        static constexpr float humanoid = 1.15f;

        float footprint = kPlayerFootprint;

        // model_size reads as an overall extent rather than a ground radius, so
        // it is compressed above the humanoid baseline and then halved. Players
        // report values that map far too tight, and their footprint barely
        // varies by race, so they take a flat figure instead.
        if (entity.is_npc) {
            const float scale = entity.model_scale > 0.0f ? entity.model_scale : 1.0f;
            float extent = entity.model_size > 0.0f ? entity.model_size * scale : humanoid;

            if (extent > humanoid) {
                extent = humanoid + std::sqrt(extent - humanoid) * 0.62f;
            }

            footprint = extent * 0.5f;
        }

        return std::fmax(std::fmin(footprint, 8.0f), 0.20f);
    }

    DWORD scale_alpha(DWORD color, float scale) const {
        const float alpha = static_cast<float>((color >> 24) & 0xFF) * scale;
        const DWORD clamped = static_cast<DWORD>(std::fmax(0.0f, std::fmin(255.0f, alpha)) + 0.5f);
        return (clamped << 24) | (color & 0x00FFFFFF);
    }

    static float clamp01(float value) {
        return std::fmax(0.0f, std::fmin(1.0f, value));
    }

    static float smoothstep(float t) {
        return t * t * (3.0f - 2.0f * t);
    }

    void probe_device(const char* reason) {
        void* device = nullptr;
        if (plugin_manager_) {
            device = plugin_manager_->GetDirect3D8Device();
        }

        d3d_device_ = static_cast<IDirect3DDevice8*>(device);

        char message[256] {};
        std::snprintf(message, sizeof(message), "device probe reason=%s manager=%p device=%p",
            reason ? reason : "unknown", static_cast<void*>(plugin_manager_), device);
        append_log(message);
    }

    bool refresh_projection_matrices() {
        if (!d3d_device_) {
            return false;
        }

        if (FAILED(d3d_device_->GetTransform(D3DTS_VIEW, &cached_view_)) ||
            FAILED(d3d_device_->GetTransform(D3DTS_PROJECTION, &cached_projection_))) {
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

    // FFXI Lua positions use x/y as the ground plane and z as height; D3D wants
    // x/z on the ground with y up.
    bool world_to_screen(const Position& point, const D3DVIEWPORT8& viewport,
        float& screen_x, float& screen_y, float& screen_rhw) const {
        if (!projection_matrices_valid_) {
            return false;
        }

        const D3DMATRIX& vp = cached_view_projection_;
        const float world_x = point.east;
        const float world_y = point.height;
        const float world_z = point.north;

        const float clip_x = world_x * vp.m[0][0] + world_y * vp.m[1][0]
            + world_z * vp.m[2][0] + vp.m[3][0];
        const float clip_y = world_x * vp.m[0][1] + world_y * vp.m[1][1]
            + world_z * vp.m[2][1] + vp.m[3][1];
        const float clip_w = world_x * vp.m[0][3] + world_y * vp.m[1][3]
            + world_z * vp.m[2][3] + vp.m[3][3];

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
        return true;
    }

    bool world_to_screen(const Position& point, const D3DVIEWPORT8& viewport,
        float& screen_x, float& screen_y) const {
        float discarded = 1.0f;
        return world_to_screen(point, viewport, screen_x, screen_y, discarded);
    }

    bool begin_draw_state() {
        if (draw_state_active_) {
            return true;
        }

        if (!d3d_device_) {
            return false;
        }

        saved_texture_ = nullptr;
        d3d_device_->GetVertexShader(&saved_shader_);
        d3d_device_->GetRenderState(D3DRS_ALPHABLENDENABLE, &saved_alpha_);
        d3d_device_->GetRenderState(D3DRS_SRCBLEND, &saved_src_);
        d3d_device_->GetRenderState(D3DRS_DESTBLEND, &saved_dest_);
        d3d_device_->GetRenderState(D3DRS_ZENABLE, &saved_z_);
        d3d_device_->GetRenderState(D3DRS_LIGHTING, &saved_lighting_);
        d3d_device_->GetRenderState(D3DRS_CULLMODE, &saved_cull_);
        d3d_device_->GetTexture(0, &saved_texture_);

        d3d_device_->SetTexture(0, nullptr);
        d3d_device_->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d3d_device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d3d_device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        d3d_device_->SetRenderState(D3DRS_ZENABLE, FALSE);
        d3d_device_->SetRenderState(D3DRS_LIGHTING, FALSE);
        d3d_device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d3d_device_->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

        draw_state_active_ = true;
        return true;
    }

    void end_draw_state() {
        if (!draw_state_active_ || !d3d_device_) {
            return;
        }

        d3d_device_->SetTexture(0, saved_texture_);
        if (saved_texture_) {
            saved_texture_->Release();
            saved_texture_ = nullptr;
        }
        d3d_device_->SetRenderState(D3DRS_ALPHABLENDENABLE, saved_alpha_);
        d3d_device_->SetRenderState(D3DRS_SRCBLEND, saved_src_);
        d3d_device_->SetRenderState(D3DRS_DESTBLEND, saved_dest_);
        d3d_device_->SetRenderState(D3DRS_ZENABLE, saved_z_);
        d3d_device_->SetRenderState(D3DRS_LIGHTING, saved_lighting_);
        d3d_device_->SetRenderState(D3DRS_CULLMODE, saved_cull_);
        d3d_device_->SetVertexShader(saved_shader_);
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

        d3d_device_->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
            static_cast<UINT>(batch_vertex_count_ / 3), batch_vertices_, sizeof(DrawVertex));
        batch_vertex_count_ = 0;
    }

    struct Locator {
        static bool page_readable(DWORD protect) {
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

        static bool span_readable(std::uintptr_t address, std::size_t size) {
            if (address == 0 || size == 0) {
                return false;
            }

            MEMORY_BASIC_INFORMATION region {};
            if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region))) {
                return false;
            }

            if (region.State != MEM_COMMIT || !page_readable(region.Protect)) {
                return false;
            }

            const std::uintptr_t low = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            return address >= low && address + size <= low + region.RegionSize;
        }

        template<typename T>
        static bool fetch(std::uintptr_t address, T& out) {
            if (!span_readable(address, sizeof(T))) {
                return false;
            }

            SIZE_T copied = 0;
            return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                &out, sizeof(T), &copied) && copied == sizeof(T);
        }

        static bool follow(std::uintptr_t address, std::uintptr_t& out, std::size_t required) {
            return fetch(address, out) && span_readable(out, required);
        }

        static bool plausible(float value, float limit) {
            return std::isfinite(value) && std::fabs(value) <= limit;
        }

        static bool entity_table(std::uintptr_t& table) {
            table = 0;

            HMODULE core = GetModuleHandleA("LuaCore.dll");
            if (!core) {
                return false;
            }

            std::uintptr_t context = 0;
            if (!fetch(reinterpret_cast<std::uintptr_t>(core) + kContextSlot, context)) {
                return false;
            }

            if (!follow(context + kTableSlot, table, sizeof(std::uintptr_t) * kMaxIndex)) {
                table = 0;
                return false;
            }

            return true;
        }

        static bool actor_for(std::uintptr_t table, DWORD index, std::uintptr_t& actor) {
            actor = 0;
            if (table == 0 || index == 0 || index >= kMaxIndex) {
                return false;
            }

            std::uintptr_t entry = 0;
            if (!follow(table + static_cast<std::uintptr_t>(index) * sizeof(std::uintptr_t),
                    entry, kEntryProbe)) {
                return false;
            }

            if (!follow(entry + kActorSlot, actor, kActorProbe)) {
                actor = 0;
                return false;
            }

            return true;
        }

        static bool ground_position(std::uintptr_t actor, Position& out) {
            float east = 0.0f;
            float height = 0.0f;
            float north = 0.0f;

            if (!fetch(actor + kRootX, east) ||
                !fetch(actor + kRootZ, height) ||
                !fetch(actor + kRootY, north)) {
                return false;
            }

            if (!plausible(east, kWorldLimit) || !plausible(north, kWorldLimit)
                || !plausible(height, kWorldLimit)) {
                return false;
            }

            out = {east, north, height};
            return true;
        }

    };

    void read_state() {
        if (!state_file_may_have_changed()) {
            return;
        }

        WIN32_FILE_ATTRIBUTE_DATA attributes {};
        if (!GetFileAttributesExA(state_path_, GetFileExInfoStandard, &attributes)) {
            return;
        }

        ULARGE_INTEGER write_time {};
        write_time.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
        write_time.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
        const unsigned long long file_size =
            (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;

        if (state_cache_valid_ && cached_write_time_ == write_time.QuadPart && cached_file_size_ == file_size) {
            return;
        }

        FILE* file = std::fopen(state_path_, "rb");
        if (!file) {
            return;
        }

        char buffer[16384] {};
        const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
        std::fclose(file);
        buffer[read] = '\0';

        std::size_t content_end = read;
        while (content_end > 0 && (buffer[content_end - 1] == ' ' || buffer[content_end - 1] == '\t'
            || buffer[content_end - 1] == '\r' || buffer[content_end - 1] == '\n')) {
            --content_end;
        }

        if (content_end == 0 || buffer[0] != '{' || buffer[content_end - 1] != '}') {
            return;
        }

        parse_player_index(buffer);
        parse_tagged_array(buffer);

        cached_write_time_ = write_time.QuadPart;
        cached_file_size_ = file_size;
        state_cache_valid_ = true;
    }

    void parse_player_index(const char* buffer) {
        player_index_ = 0;
        const char* key_pos = std::strstr(buffer, "\"player\"");
        if (!key_pos) {
            return;
        }

        const char* object = std::strchr(key_pos, '{');
        const char* tagged_key = std::strstr(buffer, "\"tagged\"");
        if (!object || (tagged_key && object > tagged_key)) {
            return;
        }

        char object_text[128] {};
        const char* object_end = std::strchr(object, '}');
        if (!object_end) {
            return;
        }

        const std::size_t length = std::min(static_cast<std::size_t>(object_end - object + 1),
            sizeof(object_text) - 1);
        std::memcpy(object_text, object, length);
        object_text[length] = '\0';
        player_index_ = parse_json_uint(object_text, "\"index\"");
        if (player_index_ >= kMaxIndex) {
            player_index_ = 0;
        }
    }

    void parse_tagged_array(const char* buffer) {
        tagged_count_ = 0;

        const char* key_pos = std::strstr(buffer, "\"tagged\"");
        if (!key_pos) {
            return;
        }

        const char* arr = std::strchr(key_pos, '[');
        if (!arr) {
            return;
        }

        const char* cursor = arr + 1;
        while (*cursor && tagged_count_ < kMaxTagged) {
            while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n' || *cursor == ',') {
                ++cursor;
            }

            if (*cursor == ']' || *cursor == '\0') {
                break;
            }

            if (*cursor != '{') {
                break;
            }

            const char* object_end = std::strchr(cursor, '}');
            if (!object_end) {
                break;
            }

            char object[512] {};
            const std::size_t length = std::min(static_cast<std::size_t>(object_end - cursor + 1), sizeof(object) - 1);
            std::memcpy(object, cursor, length);
            object[length] = '\0';

            Entity entity {};
            entity.index = parse_json_uint(object, "\"index\"");
            entity.is_npc = parse_json_bool(object, "\"npc\"");
            entity.model_size = parse_json_float(object, "\"model_size\"");
            entity.model_scale = parse_json_float(object, "\"model_scale\"");
            entity.active = parse_json_bool(object, "\"green\"");
            entity.valid = entity.index != 0 && entity.index < 0x900;
            if (entity.valid) {
                tagged_[tagged_count_++] = entity;
            }

            cursor = object_end + 1;
        }
    }

    bool state_file_may_have_changed() {
        if (!state_cache_valid_ || state_change_notification_ == INVALID_HANDLE_VALUE) {
            return true;
        }

        const DWORD wait_result = WaitForSingleObject(state_change_notification_, 0);
        if (wait_result == WAIT_TIMEOUT) {
            return false;
        }

        if (wait_result == WAIT_OBJECT_0) {
            if (!FindNextChangeNotification(state_change_notification_)) {
                close_state_change_notification();
            }
            return true;
        }

        close_state_change_notification();
        return true;
    }

    float parse_json_float(const char* start, const char* key) const {
        const char* key_pos = std::strstr(start, key);
        if (!key_pos) {
            return 0.0f;
        }

        const char* colon = std::strchr(key_pos, ':');
        return colon ? static_cast<float>(std::strtod(colon + 1, nullptr)) : 0.0f;
    }

    DWORD parse_json_uint(const char* start, const char* key) const {
        const char* key_pos = std::strstr(start, key);
        if (!key_pos) {
            return 0;
        }

        const char* colon = std::strchr(key_pos, ':');
        return colon ? static_cast<DWORD>(std::strtoul(colon + 1, nullptr, 10)) : 0;
    }

    bool parse_json_bool(const char* start, const char* key) const {
        const char* key_pos = std::strstr(start, key);
        if (!key_pos) {
            return false;
        }

        const char* colon = std::strchr(key_pos, ':');
        if (!colon) {
            return false;
        }

        while (*(++colon) == ' ') {
        }

        return std::strncmp(colon, "true", 4) == 0;
    }

    void initialize_paths_from_module() {
        char module_path[MAX_PATH] {};
        if (!g_module || !GetModuleFileNameA(g_module, module_path, sizeof(module_path))) {
            return;
        }

        char* slash = std::strrchr(module_path, '\\');
        if (!slash) {
            return;
        }

        *slash = '\0';
        char settings_root[MAX_PATH] {};
        std::snprintf(settings_root, sizeof(settings_root), "%s\\settings", module_path);
        CreateDirectoryA(settings_root, nullptr);
        std::snprintf(settings_root, sizeof(settings_root), "%s\\settings\\BubbleRings", module_path);
        CreateDirectoryA(settings_root, nullptr);
        std::snprintf(log_path_, sizeof(log_path_), "%s\\settings\\BubbleRings\\native.log", module_path);
        std::snprintf(state_path_, sizeof(state_path_), "%s\\settings\\BubbleRings\\tagged.json", module_path);
        state_change_notification_ = FindFirstChangeNotificationA(
            settings_root,
            FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    }

    void close_state_change_notification() {
        if (state_change_notification_ != INVALID_HANDLE_VALUE) {
            FindCloseChangeNotification(state_change_notification_);
            state_change_notification_ = INVALID_HANDLE_VALUE;
        }
    }

    void append_log(const char* message) {
        if (log_path_[0] == '\0') {
            return;
        }

        FILE* file = std::fopen(log_path_, "ab");
        if (!file) {
            return;
        }

        std::time_t now = std::time(nullptr);
        std::tm* local = std::localtime(&now);
        if (local) {
            char stamp[32] {};
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", local);
            std::fprintf(file, "%s %s\n", stamp, message);
        } else {
            std::fprintf(file, "%s\n", message);
        }

        std::fclose(file);
    }

    char state_path_[1024] {};
    char log_path_[1024] {};
    HANDLE state_change_notification_ = INVALID_HANDLE_VALUE;

    IDirect3DDevice8* d3d_device_ = nullptr;
    D3DMATRIX cached_view_ {};
    D3DMATRIX cached_projection_ {};
    D3DMATRIX cached_view_projection_ {};
    bool projection_matrices_valid_ = false;

    DWORD saved_shader_ = 0;
    DWORD saved_alpha_ = 0;
    DWORD saved_src_ = 0;
    DWORD saved_dest_ = 0;
    DWORD saved_z_ = 0;
    DWORD saved_lighting_ = 0;
    DWORD saved_cull_ = 0;
    IDirect3DBaseTexture8* saved_texture_ = nullptr;
    bool draw_state_active_ = false;

    DrawVertex batch_vertices_[max_batch_vertices_] {};
    int batch_vertex_count_ = 0;

    Entity tagged_[kMaxTagged] {};
    int tagged_count_ = 0;
    DWORD player_index_ = 0;
    bool player_occluder_valid_ = false;
    float player_foot_x_ = 0.0f;
    float player_foot_y_ = 0.0f;
    float player_head_x_ = 0.0f;
    float player_head_y_ = 0.0f;
    float player_half_width_ = 18.0f;
    unsigned long long cached_write_time_ = 0;
    unsigned long long cached_file_size_ = 0;
    bool state_cache_valid_ = false;

    float ring_cos_[ring_slices_ + 1] {};
    float ring_sin_[ring_slices_ + 1] {};
};

std::uint32_t GetInterfaceVersion() {
    append_module_log("GetInterfaceVersion called returning 0x04070300");
    return WINDOWER_INTERFACE_VERSION;
}

PluginBase* CreateInstance() {
    append_module_log("CreateInstance called");
    return new BubbleRingsPlugin();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        append_module_log("DllMain process attach");
    } else if (reason == DLL_PROCESS_DETACH) {
        append_module_log("DllMain process detach");
    }

    return TRUE;
}
