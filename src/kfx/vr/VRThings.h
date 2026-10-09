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
    uint8_t kind;           /**< SpriteKind. */
    int16_t one_colour;     /**< Palette index to fill the sprite with (flashing flower), -1 for none. */
    float lift;             /**< Moved this far towards the viewer, to stack layered icons. */
};

enum SpriteKind : uint8_t {
    SK_Upright = 0,  /**< A standing billboard facing the viewer. */
    SK_Shadow  = 1,  /**< A soft round shadow flat on the floor; left/right/bottom/top are its radii. */
};

/** One creature type on the palm menu. */
struct PalmItem {
    int32_t sprite_id;      /**< Its icon (panel sprite), in the sprite image cache. */
    int kind;               /**< Creature model. */
    int count;              /**< How many the player has. */
};

/** sprite_id of a plain filled quad, coloured by one_colour (flag poles, bars). */
constexpr int32_t k_solid_sprite_id = -3;

/** A decoded sprite frame: per pixel a palette index and a coverage byte. */
struct SpriteImage {
    int32_t id;
    int w, h;
    std::vector<uint8_t> rg;  /**< w*h pairs of (palette index, coverage 0/255). */
};

/** One slab of the room / dig selection the cursor box shows. */
struct SelectionTile {
    float x0, y0, x1, y1;   /**< Slab extent, game units. */
    float z;                /**< Top of what is there (floor or wall). */
    uint8_t valid;          /**< 0: inside the box but cannot be built / dug. */
};

struct ThingSnapshot {
    // Game thread -> render thread
    std::vector<SpriteBillboard> sprites;    /**< This frame's things (replaced each frame). */
    std::vector<SelectionTile> selection;    /**< The cursor box's slabs (replaced each frame). */
    std::vector<PalmItem> palm;              /**< Owned creature types, digger first (replaced each frame). */
    int32_t digit_sprite[10] = {};           /**< Number digits 0-9 for the counts. */
    float digit_aspect = 1.0f;               /**< Digit width / height. */
    std::vector<SpriteImage> new_images;     /**< Not yet uploaded; the render thread empties it. */
    // Render thread -> game thread
    bool viewer_valid = false;
    float viewer_x = 0, viewer_y = 0;        /**< Headset position in game units, for sprite facing. */
    bool reset_cache = false;                /**< Render thread lost its textures; resend all images. */
    // Game thread's own setting
    int hide_thing = 0;                      /**< Thing not to draw: the possessed creature, seen from inside. */
};

/** The creature the player is possessing (or riding along with), seen from its eyes. */
struct PossessionView {
    bool active = false;
    bool controlling = false;   /**< False when only riding along (passenger mode). */
    bool digger = false;        /**< An imp or other digger: the hand becomes a pickaxe. */
    float eye_x = 0, eye_y = 0, eye_z = 0;  /**< Game units. */
    int yaw = 0;                /**< Its facing, game angle units. */
    int thing_idx = 0;
};

/** Game thread: fill @p out from the local player's view. */
void GetPossessionView(PossessionView* out);

/** Game thread: pick up the player's next creature of @p kind into the hand,
 *  as clicking its icon in the panel's creature tab does. */
void PickUpCreatureOfKind(int kind);

/** Game thread: refresh @p snap from the map's things. */
void UpdateThingSnapshot(ThingSnapshot& snap);

} // namespace VR
