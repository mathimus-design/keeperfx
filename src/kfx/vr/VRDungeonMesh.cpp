/******************************************************************************/
// Dungeon Keeper - VR mode
/******************************************************************************/
/** @file VRDungeonMesh.cpp
 *     Builds the dungeon's walls and floors as world-space triangles.
 *     See VRDungeonMesh.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/vr/VRDungeonMesh.h"
#include "globals.h"
#include "game_legacy.h"     // game.map_subtiles_x/y, unrevealed_column_idx, top_cube, slab_ext_data
#include "map_data.h"        // get_map_block_at(), map_block_revealed(), COORD_PER_STL
#include "map_columns.h"     // struct Column, get_map_column(), cube_is_abyss()
#include "config_cubes.h"    // get_cube_model_stats()
#include "slab_data.h"       // get_slab_number()
#include "engine_arrays.h"   // floor_height_table[], lintel_top_height[]
#include "engine_textures.h" // TEXTURE_BLOCKS_COUNT
#include "frontend.h"        // frontend_menu_state
#include "player_data.h"     // my_player_number, get_my_player(), PVT_*
#include "local_camera.h"    // get_local_active_camera(), get_local_view_type()
#include "engine_camera.h"   // struct Camera
#include "dungeon_data.h"    // get_player_soul_container()
#include "thing_data.h"      // struct Thing, thing_is_invalid()
#include "config_terrain.h"  // SlbAtFlg_*
#include "roomspace_prediction.h" // get_local_dig_prediction_render_flags()
#include <cmath>
#include "post_inc.h"

namespace VR {

namespace {

constexpr float k_unit = (float)COORD_PER_STL;   // one subtile wide, one cube tall
constexpr float k_tile_uv_max = 31.999f;         // like the engine's 0x1FFFFF, stays inside the tile

// Face shading, as if lit from the front right. Multiplied by the subtile lightness.
constexpr float k_shade_top   = 1.0f;
// Negative: unlit at that brightness, ignoring the lightmap (like do_a_gpoly_unlit_*).
constexpr float k_shade_unlit = -1.0f;
// Multiplier for unexplored columns.
constexpr float k_shroud_shade = 0.12f;
constexpr float k_shade_back  = 0.60f;
constexpr float k_shade_front = 0.85f;
constexpr float k_shade_left  = 0.70f;
constexpr float k_shade_right = 0.80f;

// Chunks are hashed and rebuilt every this many snapshots.
constexpr int k_rescan_interval = 6;

struct ChunkState {
    uint64_t hash = 0;
    bool built = false;
};

std::vector<ChunkState> s_chunks;
int s_chunks_x = 0, s_chunks_y = 0;
int s_snapshot_count = 0;

const struct Column* visible_column(MapSubtlCoord stl_x, MapSubtlCoord stl_y, const struct Column* unrev)
{
    struct Map* mapblk = get_map_block_at(stl_x, stl_y);
    if (!map_block_revealed(mapblk, my_player_number))
        return unrev;
    return get_map_column(mapblk);
}

/** Map flags as the engine draws them, including dig tags the local player
 *  has placed but the server has not confirmed yet. */
unsigned char tile_render_flags(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    const struct Map* mapblk = get_map_block_at(stl_x, stl_y);
    return get_local_dig_prediction_render_flags(stl_x, stl_y, mapblk->flags);
}

/** Tile id with the slab's texture variation applied, as engine_remap_texture_blocks() does. */
unsigned short remap_texture(MapSubtlCoord stl_x, MapSubtlCoord stl_y, unsigned short tex_id)
{
    const long slb_num = get_slab_number(subtile_slab(stl_x), subtile_slab(stl_y));
    return tex_id + (game.slab_ext_data[slb_num] & 0x1F) * TEXTURE_BLOCKS_COUNT;
}

struct Corner { float x, y, z; };

/** Appends a textured quad. Corners are top-left, top-right, bottom-right,
 *  bottom-left of the texture, in subtile/cube units. */
void add_quad(std::vector<DungeonVertex>& out, unsigned short tile_id, float shade,
              Corner tl, Corner tr, Corner br, Corner bl, int max_stl_x, int max_stl_y)
{
    const Corner pos[4] = { tl, tr, br, bl };
    const float uv[4][2] = { {0, 0}, {k_tile_uv_max, 0}, {k_tile_uv_max, k_tile_uv_max}, {0, k_tile_uv_max} };
    const float layer = (float)(tile_id / TEXTURE_BLOCKS_COUNT);
    const float tile  = (float)(tile_id % TEXTURE_BLOCKS_COUNT);
    DungeonVertex v[4];
    for (int i = 0; i < 4; i++)
    {
        v[i].x = pos[i].x * k_unit;
        v[i].y = pos[i].y * k_unit;
        v[i].z = pos[i].z * k_unit;
        v[i].u = uv[i][0];
        v[i].v = uv[i][1];
        v[i].shade = shade;
        v[i].stl_x = (float)((int)pos[i].x < max_stl_x ? (int)pos[i].x : max_stl_x);
        v[i].stl_y = (float)((int)pos[i].y < max_stl_y ? (int)pos[i].y : max_stl_y);
        v[i].layer = layer;
        v[i].tile = tile;
    }
    out.push_back(v[0]); out.push_back(v[1]); out.push_back(v[2]);
    out.push_back(v[2]); out.push_back(v[3]); out.push_back(v[0]);
}

/** Mirrors do_a_plane_of_engine_columns_isometric() for one column, in world space. */
void build_column(std::vector<DungeonVertex>& out, MapSubtlCoord stl_x, MapSubtlCoord stl_y,
                  const struct Column* unrev)
{
    const int sx = game.map_subtiles_x, sy = game.map_subtiles_y;
    const struct Column* cur = visible_column(stl_x, stl_y, unrev);
    const unsigned short solid = cur->solidmask;
    // Unexplored rock sits under the shroud: nearly black, as on the desktop.
    const bool revealed = map_block_revealed(get_map_block_at(stl_x, stl_y), my_player_number);
    const float lit = revealed ? 1.0f : k_shroud_shade;

    // Faces against the map edge are always drawn, like the clipped engine path.
    unsigned short solid_back  = (stl_y > 1)      ? visible_column(stl_x, stl_y - 1, unrev)->solidmask : 0;
    unsigned short solid_front = (stl_y < sy - 1) ? visible_column(stl_x, stl_y + 1, unrev)->solidmask : 0;
    unsigned short solid_left  = (stl_x > 1)      ? visible_column(stl_x - 1, stl_y, unrev)->solidmask : 0;
    unsigned short solid_right = (stl_x < sx - 1) ? visible_column(stl_x + 1, stl_y, unrev)->solidmask : 0;

    const float x0 = (float)stl_x, x1 = x0 + 1.0f;
    const float y0 = (float)stl_y, y1 = y0 + 1.0f;

    for (int ncor = 0; ncor < 8; ncor++)
    {
        const unsigned short bit = (unsigned short)(1u << ncor);
        if ((solid & bit) == 0)
            continue;
        const struct CubeConfigStats* cube = get_cube_model_stats(cur->cubes[ncor]);
        const float z0 = (float)ncor, z1 = z0 + 1.0f;
        // Texture indices are the engine's sideoris[0]: back 0, right 1, front 2, left 3.
        if ((solid_back & bit) == 0)
            add_quad(out, remap_texture(stl_x, stl_y, cube->texture_id[0]), lit * k_shade_back,
                     {x1, y0, z1}, {x0, y0, z1}, {x0, y0, z0}, {x1, y0, z0}, sx, sy);
        if ((solid_front & bit) == 0)
            add_quad(out, remap_texture(stl_x, stl_y, cube->texture_id[2]), lit * k_shade_front,
                     {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}, sx, sy);
        if ((solid_left & bit) == 0)
            add_quad(out, remap_texture(stl_x, stl_y, cube->texture_id[3]), lit * k_shade_left,
                     {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}, sx, sy);
        if ((solid_right & bit) == 0)
            add_quad(out, remap_texture(stl_x, stl_y, cube->texture_id[1]), lit * k_shade_right,
                     {x1, y1, z1}, {x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, sx, sy);
    }

    // Top surface: the first gap above the lowest solid run, or the floor itself.
    // Tiles tagged for digging get the engine's marked textures, drawn unlit.
    const unsigned char render_flags = tile_render_flags(stl_x, stl_y);
    long top = floor_height_table[solid];
    if (top > 0)
    {
        const float z = (float)top;
        if (render_flags & SlbAtFlg_Unexplored)
        {
            add_quad(out, remap_texture(stl_x, stl_y, TEXTURE_LAND_MARKED_LAND), k_shade_unlit,
                     {x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}, sx, sy);
        }
        else if ((render_flags & SlbAtFlg_TaggedValuable) == 0)
        {
            const struct CubeConfigStats* cube = get_cube_model_stats(cur->cubes[top - 1]);
            add_quad(out, remap_texture(stl_x, stl_y, cube->texture_id[4]), lit * k_shade_top,
                     {x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}, sx, sy);
        }
        else if (render_flags & SlbAtFlg_Valuable)
        {
            add_quad(out, remap_texture(stl_x, stl_y, TEXTURE_LAND_MARKED_GOLD), k_shade_unlit,
                     {x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}, sx, sy);
        }
    }
    else if (!cube_is_abyss(game.top_cube[cur->floor_texture]))
    {
        const bool marked = (render_flags & SlbAtFlg_Unexplored) != 0;
        add_quad(out, remap_texture(stl_x, stl_y, marked ? TEXTURE_LAND_MARKED_LAND : cur->floor_texture),
                 marked ? k_shade_unlit : lit * k_shade_top,
                 {x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}, sx, sy);
    }
    // Top of a lintel above a doorway or gap.
    top = lintel_top_height[solid];
    if (top > 0)
    {
        const struct CubeConfigStats* cube = get_cube_model_stats(cur->cubes[top - 1]);
        const float z = (float)top;
        add_quad(out, remap_texture(stl_x, stl_y, cube->texture_id[4]), lit * k_shade_top,
                 {x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}, sx, sy);
    }
}

/** Range of subtiles a chunk draws, clipped to the drawable map area. */
void chunk_range(int chunk_x, int chunk_y, int* bx, int* by, int* ex, int* ey)
{
    *bx = chunk_x * k_chunk_subtiles;
    *by = chunk_y * k_chunk_subtiles;
    *ex = *bx + k_chunk_subtiles;
    *ey = *by + k_chunk_subtiles;
    if (*bx < 1) *bx = 1;
    if (*by < 1) *by = 1;
    if (*ex > game.map_subtiles_x) *ex = game.map_subtiles_x;
    if (*ey > game.map_subtiles_y) *ey = game.map_subtiles_y;
}

/** Hash of everything a chunk's geometry depends on: its columns, their
 *  neighbours one subtile out, and the slabs' texture variations. */
uint64_t chunk_hash(int chunk_x, int chunk_y, const struct Column* unrev)
{
    int bx, by, ex, ey;
    chunk_range(chunk_x, chunk_y, &bx, &by, &ex, &ey);
    uint64_t h = 1469598103934665603ull;
    for (int y = by - 1; y <= ey; y++)
    {
        for (int x = bx - 1; x <= ex; x++)
        {
            if (x < 0 || y < 0 || x > game.map_subtiles_x || y > game.map_subtiles_y)
                continue;
            const uint64_t col = (uint64_t)(uintptr_t)visible_column(x, y, unrev);
            const uint64_t variation = game.slab_ext_data[get_slab_number(subtile_slab(x), subtile_slab(y))] & 0x1F;
            const uint64_t tags = tile_render_flags(x, y) & (SlbAtFlg_Unexplored | SlbAtFlg_TaggedValuable);
            h = (h ^ col) * 1099511628211ull;
            h = (h ^ variation) * 1099511628211ull;
            h = (h ^ tags) * 1099511628211ull;
        }
    }
    return h;
}

} // namespace

bool RaycastMap(float ox, float oy, float oz, float dx, float dy, float dz, MapHit* hit)
{
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f || game.map_subtiles_x <= 0 || game.map_subtiles_y <= 0)
        return false;
    dx /= len; dy /= len; dz /= len;

    const float map_w = (float)game.map_subtiles_x * k_unit;
    const float map_h = (float)game.map_subtiles_y * k_unit;
    const struct Column* unrev = get_column(game.unrevealed_column_idx);
    // March in steps of 1/10 subtile: small enough not to skip a cube corner
    // noticeably, cheap enough to do every frame across the whole map.
    const float step = k_unit / 10.0f;
    const float max_dist = (map_w + map_h) * 2.0f;
    for (float t = 0.0f; t < max_dist; t += step)
    {
        const float x = ox + dx * t, y = oy + dy * t, z = oz + dz * t;
        if (z < 0.0f)
        {
            // Below the lowest floor (water, lava): hit at z = 0.
            if (dz >= 0.0f)
                return false;
            const float t0 = -oz / dz;
            const float fx = ox + dx * t0, fy = oy + dy * t0;
            if (fx < 0.0f || fy < 0.0f || fx >= map_w || fy >= map_h)
                return false;
            *hit = { fx, fy, 0.0f, (int)(fx / k_unit), (int)(fy / k_unit) };
            return true;
        }
        if (x < 0.0f || y < 0.0f || x >= map_w || y >= map_h)
            continue;  // outside the map, possibly still on the way in
        const int cube = (int)(z / k_unit);
        if (cube >= COLUMN_STACK_HEIGHT)
            continue;
        const int stl_x = (int)(x / k_unit), stl_y = (int)(y / k_unit);
        const struct Column* col = visible_column(stl_x, stl_y, unrev);
        if ((col->solidmask & (1u << cube)) != 0)
        {
            *hit = { x, y, z, stl_x, stl_y };
            return true;
        }
    }
    return false;
}

void UpdateDungeonSnapshot(DungeonSnapshot& snap)
{
    struct PlayerInfo* player = get_my_player();
    struct Camera* cam = (player != nullptr) ? get_local_active_camera(player) : nullptr;
    // After a level the game is back in its menus (level stats...), drawn with
    // the menu palette, while the player's view still says dungeon.
    snap.in_level = (cam != nullptr) && (get_local_view_type(player) != PVT_None)
                 && (game.map_subtiles_x > 0) && (game.map_subtiles_y > 0)
                 && (frontend_menu_state == FeSt_INITIAL);
    if (!snap.in_level)
        return;

    snap.map_w = (float)game.map_subtiles_x * k_unit;
    snap.map_h = (float)game.map_subtiles_y * k_unit;
    snap.lightmap_stride = game.map_subtiles_x + 1;
    // Centre the table on the player's Dungeon Heart. Early in a level load it
    // may not exist yet; the map centre stands in until it does (the desktop
    // camera can be anywhere then).
    const struct Thing* heart = get_player_soul_container(my_player_number);
    snap.focus_is_heart = !thing_is_invalid(heart);
    if (snap.focus_is_heart)
    {
        snap.focus_x = (float)heart->mappos.x.val;
        snap.focus_y = (float)heart->mappos.y.val;
    }
    else
    {
        snap.focus_x = snap.map_w * 0.5f;
        snap.focus_y = snap.map_h * 0.5f;
    }
    snap.focus_lightness = game.lish.subtile_lightness[get_subtile_number(
        (MapSubtlCoord)(snap.focus_x / k_unit), (MapSubtlCoord)(snap.focus_y / k_unit))];

    const int chunks_x = (game.map_subtiles_x + k_chunk_subtiles - 1) / k_chunk_subtiles;
    const int chunks_y = (game.map_subtiles_y + k_chunk_subtiles - 1) / k_chunk_subtiles;
    if (chunks_x != s_chunks_x || chunks_y != s_chunks_y)
    {
        s_chunks_x = chunks_x;
        s_chunks_y = chunks_y;
        s_chunks.assign((size_t)(chunks_x * chunks_y), ChunkState());
        s_snapshot_count = 0;
        // Pending chunks belong to the old grid; everything is rebuilt below.
        snap.updates.clear();
    }
    snap.chunks_x = chunks_x;
    snap.chunks_y = chunks_y;

    if ((s_snapshot_count++ % k_rescan_interval) != 0)
        return;

    const struct Column* unrev = get_column(game.unrevealed_column_idx);
    for (int cy = 0; cy < chunks_y; cy++)
    {
        for (int cx = 0; cx < chunks_x; cx++)
        {
            ChunkState& state = s_chunks[(size_t)(cy * chunks_x + cx)];
            const uint64_t h = chunk_hash(cx, cy, unrev);
            if (state.built && state.hash == h)
                continue;
            state.hash = h;
            state.built = true;

            ChunkUpdate update;
            update.index = cy * chunks_x + cx;
            int bx, by, ex, ey;
            chunk_range(cx, cy, &bx, &by, &ex, &ey);
            for (int y = by; y < ey; y++)
                for (int x = bx; x < ex; x++)
                    build_column(update.verts, x, y, unrev);
            snap.updates.push_back(std::move(update));
        }
    }
}

} // namespace VR
