/******************************************************************************/
// Dungeon Keeper - VR mode
/******************************************************************************/
/** @file VRThings.cpp
 *     Creatures, objects, traps and effects as upright sprites. See VRThings.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/vr/VRThings.h"
#include "globals.h"
#include "game_legacy.h"       // game.map_subtiles_x/y
#include "map_data.h"          // get_map_block_at(), map_block_revealed(), get_mapwho_thing_index()
#include "thing_data.h"        // struct Thing, thing_get(), TRF_* flags
#include "thing_list.h"        // TCls_*
#include "thing_objects.h"     // ODC_Default
#include "config_trapdoor.h"   // get_trap_model_stats()
#include "creature_graphics.h" // struct KeeperSprite, keepersprite_array(), keepersprite_rotable()
#include "engine_arrays.h"     // get_render_animation_sprite()
#include "engine_render.h"     // resolve_keepersprite_draw_data(), interpolate_thing()
#include "player_data.h"       // my_player_number, get_my_player()
#include "dungeon_data.h"      // get_my_dungeon(), things_in_hand
#include <cmath>
#include <unordered_set>
#include "post_inc.h"

namespace VR {

namespace {

/** Sprite ids whose images the render thread already has. */
std::unordered_set<int32_t> s_sent_images;

/** Decodes TbSprite RLE: negative = transparent skip, positive = run of
 *  palette indices, 0 = end of row. Same format decode_keeper_rle() reads. */
void decode_sprite(SpriteImage& img, const unsigned char* data)
{
    img.rg.assign((size_t)img.w * img.h * 2, 0);
    const signed char* sp = (const signed char*)data;
    const signed char* sp_end = sp + (ptrdiff_t)img.w * img.h * 3 + img.h;  // generous bound
    for (int y = 0; y < img.h; y++)
    {
        uint8_t* row = &img.rg[(size_t)y * img.w * 2];
        int x = 0;
        while (sp < sp_end)
        {
            const signed char cmd = *sp++;
            if (cmd == 0)
                break;
            if (cmd < 0)
            {
                x -= cmd;
                continue;
            }
            for (int i = 0; i < cmd && sp < sp_end; i++, sp++, x++)
            {
                if (x < img.w)
                {
                    row[x * 2] = (uint8_t)*sp;
                    row[x * 2 + 1] = 255;
                }
            }
        }
    }
}

/** Game angle (0..2047, 0 = north / -y, clockwise) of the direction from (fx, fy) to (tx, ty). */
long angle_between(float fx, float fy, float tx, float ty)
{
    const double a = std::atan2((double)(tx - fx), (double)(fy - ty));  // east positive, north positive
    long ang = (long)std::lround(a * 1024.0 / 3.14159265358979);
    return ang & ANGLE_MASK;
}

bool should_draw(const struct Thing* thing, const struct PlayerInfo* player)
{
    if ((thing->rendering_flags & TRF_Invisible) != 0 || thing->draw_class != ODC_Default)
        return false;
    if (thing->class_id == TCls_Trap)
    {
        // Hidden traps stay hidden from other players until revealed, as in draw_jonty_mapwho().
        const struct TrapConfigStats* trapst = get_trap_model_stats(thing->model);
        if (trapst->hidden == 1 && player->id_number != thing->owner && thing->trap.revealed == 0)
            return false;
    }
    return true;
}

/** @param hand_slot -1 for a thing on the map, else its place in the Keeper's hand. */
void add_thing(ThingSnapshot& snap, struct Thing* thing, int hand_slot)
{
    const bool in_hand = hand_slot >= 0;
    const unsigned short anim = get_render_animation_sprite(thing->anim_sprite);
    const struct KeeperSprite* base = keepersprite_array(anim);
    if (base == nullptr || base->FramesCount == 0)
        return;

    const struct ThingInterpolateResult interp = interpolate_thing(thing);
    const float px = (float)interp.mappos.x.val;
    const float py = (float)interp.mappos.y.val;
    const float pz = (float)interp.mappos.z.val;

    // Facing relative to the viewer, as draw_jonty_mapwho() does with the camera angle.
    long angle = thing->move_angle_xy;
    if (in_hand)
    {
        angle = DEGREES_180;  // held things face the Keeper
    }
    else if (keepersprite_rotable(anim))
    {
        const long view_angle = snap.viewer_valid ? angle_between(snap.viewer_x, snap.viewer_y, px, py) : 0;
        angle = thing->move_angle_xy - view_angle;
        angle += DEGREES_45 * (long)((thing->flags & TAF_ROTATED_MASK) >> TAF_ROTATED_SHIFT);
    }
    angle &= ANGLE_MASK;

    int32_t draw_idx;
    const unsigned char* data;
    int src_w, src_h;
    const struct KeeperSprite* kspr = nullptr;
    if (!resolve_keepersprite_draw_data(anim, (short)angle, thing->current_frame, &draw_idx, &data, &src_w, &src_h, &kspr))
        return;
    if (src_w <= 0 || src_h <= 0)
        return;

    if (s_sent_images.insert(draw_idx).second)
    {
        SpriteImage img;
        img.id = draw_idx;
        img.w = src_w;
        img.h = src_h;
        decode_sprite(img, data);
        snap.new_images.push_back(std::move(img));
    }

    // Placement, mirroring process_keeper_sprite(): offsets are in sprite
    // pixels from the thing's position, y down; one sprite pixel is
    // sprite_size/32 game units (the iso view draws both at the same zoom).
    const bool flip = (base->Rotable == 2) && (angle > 1151) && (angle < 1919);
    float left_px;
    if (flip)
        left_px = (float)(-base->offset_x - kspr->FrameOffsW - src_w);
    else
        left_px = (float)(base->offset_x + kspr->FrameOffsW);
    const float top_px = (float)(base->offset_y + kspr->FrameOffsH);
    const float unit = (float)thing->sprite_size / 32.0f;

    SpriteBillboard b;
    b.x = px;
    b.y = py;
    b.z = pz;
    b.left = left_px * unit;
    b.right = (left_px + src_w) * unit;
    b.top = -top_px * unit;
    b.bottom = -(top_px + src_h) * unit;
    b.stl_x = in_hand ? -1.0f : (float)interp.mappos.x.stl.num;  // -1: full brightness
    b.stl_y = in_hand ? -1.0f : (float)interp.mappos.y.stl.num;
    b.in_hand = in_hand ? 1 : 0;
    b.hand_slot = in_hand ? (uint8_t)hand_slot : 0;
    b.sprite_id = draw_idx;
    b.flip = flip ? 1 : 0;
    switch (thing->rendering_flags & TRF_Transpar_Flags)
    {
    case TRF_Transpar_8:
    case TRF_Transpar_4:
        b.blend = 1;
        break;
    case TRF_Transpar_Alpha:
        b.blend = 2;
        break;
    default:
        b.blend = 0;
        break;
    }
    snap.sprites.push_back(b);
}

} // namespace

void UpdateThingSnapshot(ThingSnapshot& snap)
{
    if (snap.reset_cache)
    {
        s_sent_images.clear();
        snap.new_images.clear();
        snap.reset_cache = false;
    }
    snap.sprites.clear();

    struct PlayerInfo* player = get_my_player();
    if (player == nullptr || game.map_subtiles_x <= 0 || game.map_subtiles_y <= 0)
        return;

    for (MapSubtlCoord stl_y = 0; stl_y < game.map_subtiles_y; stl_y++)
    {
        for (MapSubtlCoord stl_x = 0; stl_x < game.map_subtiles_x; stl_x++)
        {
            struct Map* mapblk = get_map_block_at(stl_x, stl_y);
            if (!map_block_revealed(mapblk, my_player_number))
                continue;
            long i = get_mapwho_thing_index(mapblk);
            unsigned long guard = 0;
            while (i > 0 && guard++ < THINGS_COUNT)
            {
                struct Thing* thing = thing_get(i);
                if (thing_is_invalid(thing))
                    break;
                i = thing->next_on_mapblk;
                if (should_draw(thing, player))
                    add_thing(snap, thing, -1);
            }
        }
    }

    // What the Keeper's hand holds is off the map; the render thread hangs it
    // from the controller.
    struct Dungeon* dungeon = get_my_dungeon();
    if (!dungeon_invalid(dungeon))
    {
        const int count = dungeon->num_things_in_hand;
        for (int slot = 0; slot < count && slot < MAX_THINGS_IN_HAND; slot++)
        {
            struct Thing* thing = thing_get(dungeon->things_in_hand[slot]);  // [0] is dropped next
            if (!thing_is_invalid(thing))
                add_thing(snap, thing, slot);
        }
    }
}

} // namespace VR
