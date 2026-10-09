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
#include "local_camera.h"      // get_local_view_type()
#include "dungeon_data.h"      // get_my_dungeon(), things_in_hand
#include "creature_control.h"  // struct CreatureControl, creature_control_get_from_thing()
#include "config_creature.h"   // creature_stats_get_from_thing(), get_creature_model_flags(), CMF_*
#include "creature_states.h"   // creature_is_being_unconscious()
#include "creature_states_combt.h" // creature_would_benefit_from_healing()
#include "creature_states_lair.h"  // creature_can_do_healing_sleep()
#include "thing_creature.h"    // creature_is_invisible(), get_creature_lair_room(), thing_is_creature()
#include "room_data.h"         // room_is_invalid()
#include "custom_sprites.h"    // get_button_sprite(), get_button_sprite_for_player()
#include "sprites.h"           // GBS_*
#include "gui_draw.h"          // gui_blink_rate, neutral_flash_rate
#include "player_instances.h"  // PLAYER_NEUTRAL
#include "keeperfx.hpp"        // MFlg_NoHeroHealthFlower
#include "kfx/renderer/RendererManager.h" // RendererGetDrawFlags()
#include "vidfade.h"           // colours
#include "config_settings.h"   // settings.roomflags_on
#include "config_terrain.h"    // room_can_have_ensign(), get_room_kind_stats()
#include "config_magic.h"      // struct RoomConfigStats medsym_sprite_idx
#include "roomspace.h"         // struct RoomSpace, MAX_ROOMSPACE_WIDTH
#include "roomspace_prediction.h" // get_local_dig_prediction_render_roomspace()
#include "frontend.h"          // game_is_busy_doing_gui()
#include "map_columns.h"       // get_column(), get_map_column()
#include "engine_arrays.h"     // floor_height_table[]
#include <cmath>
#include <unordered_set>
#include "post_inc.h"

/* engine_render.c; not in a header. */
extern "C" void fill_status_sprite_indexes(struct Thing *thing, struct CreatureControl *cctrl, short *health_spridx,
                                           short *state_spridx, short *anger_spridx);
extern "C" TbBool cursor_on_room(RoomIndex room_index);
extern "C" TbBool room_is_damaged(RoomIndex room_index);
extern "C" TbBool placing_same_room_type(RoomIndex room_index);

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

/** Cache id for a GUI sprite: they are not keeper sprites, so keep them in
 *  their own id range, keyed by the sprite (they stay loaded for the level). */
int32_t gui_sprite_id(const struct TbSprite* spr)
{
    return (int32_t)(0x40000000u | (uint32_t)(((uintptr_t)spr >> 3) & 0x3FFFFFFFu));
}

/** Adds an upright, full-brightness GUI sprite above a creature. */
void add_status_layer(ThingSnapshot& snap, const struct TbSprite* spr, float x, float y, float z,
                      float unit, int16_t one_colour, uint8_t blend, float lift)
{
    if (spr == nullptr || spr->Data == nullptr || spr->SWidth == 0 || spr->SHeight == 0)
        return;
    const int32_t id = gui_sprite_id(spr);
    if (s_sent_images.insert(id).second)
    {
        SpriteImage img;
        img.id = id;
        img.w = spr->SWidth;
        img.h = spr->SHeight;
        decode_sprite(img, spr->Data);
        snap.new_images.push_back(std::move(img));
    }
    SpriteBillboard b = {};
    b.x = x;
    b.y = y;
    b.z = z;
    b.left = -0.5f * spr->SWidth * unit;
    b.right = 0.5f * spr->SWidth * unit;
    b.bottom = 0.0f;
    b.top = spr->SHeight * unit;
    b.stl_x = b.stl_y = -1.0f;  // full brightness, as on the desktop
    b.sprite_id = id;
    b.blend = blend;
    b.kind = SK_Upright;
    b.one_colour = one_colour;
    b.lift = lift;
    snap.sprites.push_back(b);
}

/** The health flower, level and thought bubble above a creature. Mirrors
 *  draw_status_sprites() (which also decides when each is shown), placed in
 *  the world above the creature's head as add_draw_status_box() does. */
void add_status(ThingSnapshot& snap, struct Thing* thing, float px, float py, float pz)
{
    struct PlayerInfo* player = get_my_player();
    struct CreatureControl* cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl))
        return;
    if (cctrl->force_health_flower_hidden || flag_is_set(get_creature_model_flags(thing), CMF_NoHealthFlower))
        return;
    if (flag_is_set(game.mode_flags, MFlg_NoHeroHealthFlower))
    {
        if (local_state.local_thing_under_hand != thing->index)
        {
            cctrl->thought_bubble_last_turn_drawn = get_gameturn();
            if (!cctrl->force_health_flower_displayed)
                return;
        }
        cctrl->thought_bubble_display_timer = 40;
    }

    short health_spridx = 0, state_spridx = 0, anger_spridx = 0;
    const unsigned short draw_flags = RendererGetDrawFlags();  // fill_status_sprite_indexes() sets some
    fill_status_sprite_indexes(thing, cctrl, &health_spridx, &state_spridx, &anger_spridx);
    RendererSetDrawFlags(draw_flags);
    const int exp_level = (cctrl->exp_level < 9) ? cctrl->exp_level : 9;

    // Height above the creature, as add_draw_status_box() works it out.
    const struct CreatureModelConfig* crconf = creature_stats_get_from_thing(thing);
    long offset = thing->clipbox_size_z + crconf->status_offset;
    offset += (offset * game.conf.crtr_conf.exp.size_increase_on_exp * cctrl->exp_level) / 100;
    const float z = pz + (float)offset;

    // GUI sprite pixels to game units: the level flower is about two thirds of a subtile wide.
    const struct TbSprite* level_spr = get_button_sprite(GBS_creature_flower_level_01 + exp_level);
    if (level_spr == nullptr || level_spr->SWidth == 0)
        return;
    const float unit = 170.0f / (float)level_spr->SWidth;
    const int blink = (gui_blink_rate > 0) ? gui_blink_rate : 1;
    const GameTurn turn = get_gameturn();

    // Each layer is nudged a little further towards the viewer so stacked
    // icons do not fight over the same depth.
    float lift = 4.0f;
    float raise = 0.0f;
    if (state_spridx || anger_spridx)
    {
        add_status_layer(snap, get_button_sprite(GBS_creature_states_cloud), px, py, z, unit, -1,
                         is_my_player_number(thing->owner) ? 1 : 0, lift);
        lift += 4.0f;
    }
    if (((turn % (8 * blink)) < 4 * (GameTurn)blink) && (anger_spridx > 0))
    {
        add_status_layer(snap, get_button_sprite(anger_spridx), px, py, z, unit, -1, 0, lift);
        lift += 4.0f;
        if (state_spridx)
        {
            const struct TbSprite* spr = get_button_sprite_for_player(state_spridx, thing->owner);
            if (spr != nullptr)
                raise += spr->SHeight * unit;
        }
    }
    else if (state_spridx)
    {
        const struct TbSprite* spr = get_button_sprite_for_player(state_spridx, thing->owner);
        add_status_layer(snap, spr, px, py, z, unit, -1, 0, lift);
        lift += 4.0f;
        if (spr != nullptr)
            raise += spr->SHeight * unit;
    }

    if ((thing->lair.spr_size > 0) && (health_spridx > 0) && ((turn % (2 * blink)) >= (GameTurn)blink))
    {
        // Flashing in the owner's colour.
        int flash_color = get_player_color_idx(thing->owner);
        if (flash_color == PLAYER_NEUTRAL)
            flash_color = (int)((turn % (4 * neutral_flash_rate)) / neutral_flash_rate);
        add_status_layer(snap, get_button_sprite_for_player(health_spridx, thing->owner), px, py, z + raise, unit,
                         (int16_t)player_flash_colours[flash_color], 0, lift);
        return;
    }

    const TbBool is_under_hand = (local_state.local_thing_under_hand == thing->index);
    const TbBool is_enemy_and_visible = players_are_enemies(player->id_number, thing->owner) && !creature_is_invisible(thing);
    TbBool is_owned_and_hurt = false;
    TbBool is_allied = false;
    TbBool should_drag_to_lair = false;
    if (!is_enemy_and_visible)
    {
        is_owned_and_hurt = creature_would_benefit_from_healing(thing) && !creature_is_being_unconscious(thing)
                         && (player->id_number == thing->owner);
        is_allied = players_are_mutual_allies(player->id_number, thing->owner) && (player->id_number != thing->owner);
        should_drag_to_lair = creature_is_being_unconscious(thing) && (player->id_number == thing->owner)
            && ((game.conf.rules[thing->owner].workers.drag_to_lair == 1 && !room_is_invalid(get_creature_lair_room(thing)))
             || (game.conf.rules[thing->owner].workers.drag_to_lair == 2 && creature_can_do_healing_sleep(thing)));
    }
    const TbBool is_zombie_player = !flag_is_set(get_player(thing->owner)->allocflags, PlaF_Allocated);
    if (cctrl->force_health_flower_displayed || is_under_hand || is_enemy_and_visible || is_owned_and_hurt
        || is_allied || is_zombie_player || (thing->owner == PLAYER_NEUTRAL) || should_drag_to_lair
        || (cctrl->combat_flags != 0) || (thing->lair.spr_size > 0))
    {
        if (health_spridx > 0)
        {
            add_status_layer(snap, get_button_sprite_for_player(health_spridx, thing->owner), px, py, z + raise, unit, -1, 0, lift);
            lift += 4.0f;
        }
        add_status_layer(snap, level_spr, px, py, z + raise, unit, -1, 0, lift);
    }
}

/** A plain filled upright rectangle in one palette colour. Extents are
 *  relative to the anchor as for any billboard. */
void add_solid(ThingSnapshot& snap, float x, float y, float z, float left, float right, float bottom, float top,
               TbPixel colour, float lift)
{
    SpriteBillboard b = {};
    b.x = x; b.y = y; b.z = z;
    b.left = left; b.right = right; b.bottom = bottom; b.top = top;
    b.stl_x = b.stl_y = -1.0f;
    b.sprite_id = k_solid_sprite_id;
    b.kind = SK_Upright;
    b.one_colour = colour;
    b.lift = lift;
    snap.sprites.push_back(b);
}

/** Adds a GUI sprite with its top-left corner at (left, top) around the anchor. */
void add_overlay_sprite(ThingSnapshot& snap, const struct TbSprite* spr, float x, float y, float z,
                        float left, float top, float unit, float lift)
{
    if (spr == nullptr || spr->Data == nullptr || spr->SWidth == 0 || spr->SHeight == 0)
        return;
    add_status_layer(snap, spr, x, y, z, unit, -1, 0, lift);
    SpriteBillboard& b = snap.sprites.back();
    b.left = left;
    b.right = left + spr->SWidth * unit;
    b.top = top;
    b.bottom = top - spr->SHeight * unit;
}

/** Floating gold when buying or selling: the number's digits, as
 *  draw_engine_number() lays them out. */
void add_price(ThingSnapshot& snap, struct Thing* thing)
{
    long number = thing->price_effect.number;
    if (number <= 0)
        return;
    const struct ThingInterpolateResult interp = interpolate_thing(thing);
    const struct TbSprite* digit0 = get_button_sprite(GBS_fontchars_number_dig0);
    if (digit0 == nullptr || digit0->SWidth == 0)
        return;
    constexpr float unit = 9.0f;  // game units per GUI pixel
    const float w = digit0->SWidth * unit;
    int ndigits = 0;
    for (long n = number; n > 0; n /= 10)
        ndigits++;
    // Rightmost digit first, centred on the thing.
    float left = w * (float)(ndigits - 1) * 0.5f - w * 0.5f;
    for (long n = number; n > 0; n /= 10)
    {
        add_overlay_sprite(snap, get_button_sprite((short)((n % 10) + GBS_fontchars_number_dig0)),
                           (float)interp.mappos.x.val, (float)interp.mappos.y.val, (float)interp.mappos.z.val,
                           left, digit0->SHeight * unit, unit, 6.0f);
        left -= w;
    }
}

/** A room's flag: pole, ensign with the room's symbol and its health,
 *  capacity and efficiency bars. Shown when draw_engine_room_flagpole() /
 *  draw_room_flag_top() would show it, which do_map_who_for_thing() decides. */
void add_room_flag(ThingSnapshot& snap, struct Thing* thing)
{
    if (!settings.roomflags_on)
        return;
    const RoomIndex room_idx = thing->lair.belongs_to;
    if (!cursor_on_room(room_idx) && !room_is_damaged(room_idx) && !placing_same_room_type(room_idx))
        return;
    // The flag rises once it has been asked for 10 turns in a row; shared with
    // the desktop renderer, which keeps the same count once per turn.
    const GameTurn turn = get_gameturn();
    if (turn - thing->roomflag.last_turn_drawn == 1)
    {
        if (thing->roomflag.display_timer < 10)
            thing->roomflag.display_timer++;
    }
    else if (turn - thing->roomflag.last_turn_drawn > 1)
    {
        thing->roomflag.display_timer = 0;
    }
    thing->roomflag.last_turn_drawn = turn;
    if (thing->roomflag.display_timer != 10)
        return;
    struct Room* room = room_get(thing->roomflag.room_idx);
    if (!room_exists(room) || !room_can_have_ensign(room->kind))
        return;

    const struct ThingInterpolateResult interp = interpolate_thing(thing);
    const float x = (float)interp.mappos.x.val, y = (float)interp.mappos.y.val, z = (float)interp.mappos.z.val;
    // Iso view sizes in game units: the pole's top is 1024 above the flag's
    // position. GUI parts are in "base units" (a 16-unit-per-pixel UI pixel).
    constexpr float k_pole_top = 1024.0f;
    constexpr float k_base = 5.5f;
    add_solid(snap, x, y, z, 0.0f, 4.0f * k_base, 0.0f, k_pole_top, colours[3][1][0], 0.0f);
    add_solid(snap, x, y, z, 2.0f * k_base, 3.0f * k_base, 0.0f, k_pole_top, colours[1][0][0], 2.0f);

    const struct TbSprite* ensign = get_panel_sprite(GPS_rpanel_room_ensign_filled);
    if (ensign == nullptr || ensign->SHeight == 0)
        return;
    // The ensign is drawn 36 base units tall; its own pixels scale to match.
    const float unit = 36.0f * k_base / (float)ensign->SHeight;
    add_overlay_sprite(snap, ensign, x, y, z, 0.0f, k_pole_top, unit, 4.0f);
    const struct RoomConfigStats* roomst = get_room_kind_stats(room->kind);
    add_overlay_sprite(snap, get_panel_sprite(roomst->medsym_sprite_idx), x, y, z,
                       -2.0f * k_base, k_pole_top + 4.0f * k_base, unit, 6.0f);

    // Bars: the ensign has them full; black boxes cover the empty part from the right.
    const int bar_full = 10;  // ROOM_FLAG_PROGRESS_BAR_WIDTH in engine_render.c
    int empty[3] = { 0, 0, 0 };
    if (room->slabs_count > 0)
        empty[0] = bar_full - (int)(bar_full * room->health / compute_room_max_health(room->slabs_count, room->efficiency));
    if (room->total_capacity > 0)
        empty[1] = bar_full - (int)(bar_full * room->used_capacity / room->total_capacity);
    empty[2] = bar_full - (int)(bar_full * room->efficiency / ROOM_EFFICIENCY_MAX);
    const float bar_right = ensign->SWidth * unit - 0.5f * k_base;
    for (int i = 0; i < 3; i++)
    {
        if (empty[i] <= 0)
            continue;
        const float top = k_pole_top - (8.0f * (float)(i + 1)) * k_base;
        add_solid(snap, x, y, z, bar_right - 2.0f * empty[i] * k_base, bar_right, top - 5.0f * k_base, top,
                  colours[0][0][0], 8.0f);
    }
}

/** The slabs of the box the desktop draws around the cursor while placing
 *  rooms, selling or tagging for digging (process_isometric_map_volume_box()). */
void add_selection(ThingSnapshot& snap, struct PlayerInfo* player)
{
    if (!map_volume_box.visible || game_is_busy_doing_gui())
        return;
    const struct RoomSpace* rs = get_local_dig_prediction_render_roomspace(&player->render_roomspace);
    const int width = (rs->width < MAX_ROOMSPACE_WIDTH) ? rs->width : MAX_ROOMSPACE_WIDTH;
    const int height = (rs->height < MAX_ROOMSPACE_WIDTH) ? rs->height : MAX_ROOMSPACE_WIDTH;
    const struct Column* unrev = get_column(game.unrevealed_column_idx);
    const float slab = (float)(STL_PER_SLB * COORD_PER_STL);
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            const bool valid = rs->slab_grid[x][y];
            if (!valid && !rs->render_roomspace_as_box)
                continue;  // the "automagic" shape shows only the slabs it covers
            const MapSlabCoord slb_x = rs->left + x, slb_y = rs->top + y;
            if (slb_x < 0 || slb_y < 0 || slb_x >= game.map_tiles_x || slb_y >= game.map_tiles_y)
                continue;
            // Sit on top of whatever the slab's middle subtile shows.
            struct Map* mapblk = get_map_block_at(slab_subtile_center(slb_x), slab_subtile_center(slb_y));
            const struct Column* col = map_block_revealed(mapblk, my_player_number) ? get_map_column(mapblk) : unrev;
            long top = floor_height_table[col->solidmask];
            SelectionTile t;
            t.x0 = slb_x * slab;
            t.y0 = slb_y * slab;
            t.x1 = t.x0 + slab;
            t.y1 = t.y0 + slab;
            t.z = (float)(top * COORD_PER_STL);
            t.valid = valid ? 1 : 0;
            snap.selection.push_back(t);
        }
    }
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
    b.kind = SK_Upright;
    b.one_colour = -1;
    b.lift = 0.0f;
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

    if (!in_hand && thing_is_creature(thing))
    {
        // A soft shadow on the floor, under creatures that cast one.
        if ((base->frame_flags & FFL_NoShadows) == 0 && (thing->movement_flags & TMvF_BeingSacrificed) == 0)
        {
            SpriteBillboard shadow = b;
            const float radius = std::fmax(40.0f, 0.45f * (b.right - b.left));
            shadow.kind = SK_Shadow;
            shadow.z = (float)interp.floor_height + 3.0f;  // just above the floor, at any jump height
            shadow.left = -radius;
            shadow.right = radius;
            shadow.bottom = -radius * 0.75f;
            shadow.top = radius * 0.75f;
            shadow.blend = 0;
            snap.sprites.push_back(shadow);
        }
        add_status(snap, thing, px, py, pz);
    }
}

} // namespace

void GetPossessionView(PossessionView* out)
{
    *out = PossessionView();
    struct PlayerInfo* player = get_my_player();
    if (player == nullptr)
        return;
    const unsigned char view = get_local_view_type(player);
    if (view != PVT_CreatureContrl && view != PVT_CreaturePasngr)
        return;
    struct Thing* creature = thing_get(player->controlled_thing_idx);
    if (!thing_exists(creature) || !thing_is_creature(creature))
        return;
    const struct ThingInterpolateResult interp = interpolate_thing(creature);
    out->active = true;
    out->controlling = (view == PVT_CreatureContrl);
    out->eye_x = (float)interp.mappos.x.val;
    out->eye_y = (float)interp.mappos.y.val;
    out->eye_z = (float)(interp.mappos.z.val + get_creature_eye_height(creature));
    out->yaw = creature->move_angle_xy & ANGLE_MASK;
    out->thing_idx = creature->index;
    out->digger = thing_is_creature_digger(creature);
}

void UpdateThingSnapshot(ThingSnapshot& snap)
{
    if (snap.reset_cache)
    {
        s_sent_images.clear();
        snap.new_images.clear();
        snap.reset_cache = false;
    }
    snap.sprites.clear();
    snap.selection.clear();

    struct PlayerInfo* player = get_my_player();
    if (player == nullptr || game.map_subtiles_x <= 0 || game.map_subtiles_y <= 0)
        return;
    add_selection(snap, player);

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
                if ((thing->rendering_flags & TRF_Invisible) != 0 || thing->index == snap.hide_thing)
                    continue;
                if (thing->draw_class == ODC_RoomPrice)
                    add_price(snap, thing);
                else if (thing->draw_class == ODC_RoomStatusFlag)
                    add_room_flag(snap, thing);
                else if (should_draw(thing, player))
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
