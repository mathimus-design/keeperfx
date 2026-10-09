/******************************************************************************/
// Dungeon Keeper - VR mode
/******************************************************************************/
/** @file VRThings.h
 *     Creatures, objects, traps and effects as upright sprites on the tabletop.
 * @par Purpose:
 *     Each frame the game thread walks the revealed map's things, the same set
 *     do_map_who() draws, and picks every thing's sprite frame as seen from the
 *     headset's position (so a creature shows its back when walking away from
 *     you). Sprite images are decoded once and handed to the render thread,
 *     which caches them as textures by sprite id.
 */
/******************************************************************************/
#pragma once

#include <cstdint>
#include <vector>

namespace VR {

/** One thing to draw, in game units (x/y on the map, z up). */
struct SpriteBillboard {
    float x, y, z;          /**< Anchor: the thing's position. */
    float left, right;      /**< Horizontal extent around the anchor, along the billboard's right. */
    float bottom, top;      /**< Vertical extent around the anchor, up positive. */
    float stl_x, stl_y;     /**< Subtile whose lightness lights the sprite. */
    int32_t sprite_id;      /**< Key into the sprite image cache. */
    uint8_t flip;           /**< Mirror the image horizontally. */
    uint8_t blend;          /**< 0 opaque, 1 half transparent, 2 additive. */
    uint8_t in_hand;        /**< Held by the Keeper: drawn at the controller, not at x/y/z. */
    uint8_t hand_slot;      /**< Position in the hand, 0 = dropped next. */
};

/** A decoded sprite frame: per pixel a palette index and a coverage byte. */
struct SpriteImage {
    int32_t id;
    int w, h;
    std::vector<uint8_t> rg;  /**< w*h pairs of (palette index, coverage 0/255). */
};

struct ThingSnapshot {
    // Game thread -> render thread
    std::vector<SpriteBillboard> sprites;    /**< This frame's things (replaced each frame). */
    std::vector<SpriteImage> new_images;     /**< Not yet uploaded; the render thread empties it. */
    // Render thread -> game thread
    bool viewer_valid = false;
    float viewer_x = 0, viewer_y = 0;        /**< Headset position in game units, for sprite facing. */
    bool reset_cache = false;                /**< Render thread lost its textures; resend all images. */
};

/** Game thread: refresh @p snap from the map's things. */
void UpdateThingSnapshot(ThingSnapshot& snap);

} // namespace VR
