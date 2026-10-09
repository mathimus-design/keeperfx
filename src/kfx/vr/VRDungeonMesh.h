/******************************************************************************/
// Dungeon Keeper - VR mode
/******************************************************************************/
/** @file VRDungeonMesh.h
 *     Builds the dungeon's walls and floors as world-space triangles.
 * @par Purpose:
 *     The normal renderer only produces geometry already projected for the
 *     desktop camera, and only for what that camera sees. VR needs the whole
 *     map in world space, so this walks the map columns directly, the same way
 *     do_a_plane_of_engine_columns_isometric() does, and emits textured
 *     triangles in game units (x, y on the map, z up; 256 units per subtile
 *     and per cube).
 *
 *     The map is split into chunks so only the chunks whose columns changed
 *     (digging, claiming, revealing) are rebuilt.
 */
/******************************************************************************/
#pragma once

#include <cstdint>
#include <vector>

namespace VR {

/** One vertex of the VR dungeon mesh. Matches the VR world vertex shader. */
struct DungeonVertex {
    float x, y, z;      /**< Game world units: x/y on the map, z up. */
    float u, v;         /**< Texel coordinates within the 32x32 tile. */
    float shade;        /**< Face shading factor, multiplied by the lightmap. */
    float stl_x, stl_y; /**< Subtile whose lightness lights this vertex. */
    float layer;        /**< Tile atlas layer (texture variation). */
    float tile;         /**< Tile index within the atlas layer. */
};

constexpr int k_chunk_subtiles = 16;

/** A rebuilt chunk, handed from the game thread to the render thread. */
struct ChunkUpdate {
    int index;                         /**< chunk_y * chunks_x + chunk_x */
    std::vector<DungeonVertex> verts;
};

/** What the game thread hands to the render thread each frame. */
struct DungeonSnapshot {
    bool in_level = false;
    int chunks_x = 0, chunks_y = 0;    /**< Chunk grid size; a change means a new map. */
    int lightmap_stride = 0;           /**< Subtiles per lightness row (map_subtiles_x + 1). */
    float map_w = 0, map_h = 0;        /**< Map size in game units. */
    float focus_x = 0, focus_y = 0;    /**< Table centre: the Dungeon Heart, else the map centre. */
    bool focus_is_heart = false;       /**< False until the heart exists (early in level load). */
    int focus_lightness = 0;           /**< Subtile lightness under the focus (diagnostics). */
    std::vector<ChunkUpdate> updates;  /**< Rebuilt chunks not yet taken by the render thread. */
};

/** Where a ray meets the dungeon. */
struct MapHit {
    float x, y, z;       /**< Hit point, game units. */
    int stl_x, stl_y;    /**< Subtile that was hit (a wall's own subtile for wall faces). */
};

/** Game thread: casts a ray (game units) against the visible map: solid
 *  cubes of revealed columns, unrevealed rock, and the floor. */
bool RaycastMap(float ox, float oy, float oz, float dx, float dy, float dz, MapHit* hit);

/** Game thread: refresh @p snap from the current map. Rebuilds changed
 *  chunks every few calls and appends them to snap.updates; the render
 *  thread empties that list once it has uploaded them. */
void UpdateDungeonSnapshot(DungeonSnapshot& snap);

} // namespace VR
