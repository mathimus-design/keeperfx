/******************************************************************************/
// Dungeon Keeper - VR mode
/******************************************************************************/
/** @file VRInput.cpp
 *     The right controller as the Keeper's hand. See VRInput.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/vr/VRInput.h"
#include "kfx/vr/VRDungeonMesh.h"
#include "globals.h"
#include "keeperfx.hpp"      // pointer_x/y, block/top_pointed_at_*, me_pointed_at
#include "engine_render.h"   // floor_pointed_at_x/y
#include "map_data.h"        // get_map_block_at()
#include "player_data.h"     // local_state.engine_window_*
#include "bflib_video.h"     // lbDisplay, pixel_size
#include "bflib_mouse.h"     // LbMouseSetPositionInitial()
#include "frontend.h"        // status_panel_width
#include "game_legacy.h"     // game.operation_flags, GOF_ShowGui
#include "config_players.h"  // get_player_state_stats(), PsPg_Spell, PSt_*
#include "packets.h"         // set_players_packet_action(), PckA_SetPlyrState
#include "post_inc.h"

namespace VR {

namespace {

bool s_left_down = false;
bool s_right_down = false;

/** Same state changes mouseControl() makes for a button press / release,
 *  at the current mouse position. */
void set_left_button(bool down)
{
    if (down)
    {
        lbDisplay.MLeftButton = 1;
        if (!lbDisplay.LeftButton)
        {
            lbDisplay.MouseX = lbDisplay.MMouseX;
            lbDisplay.MouseY = lbDisplay.MMouseY;
            lbDisplay.RLeftButton = 0;
            lbDisplay.LeftButton = 1;
        }
    }
    else
    {
        lbDisplay.MLeftButton = 0;
        if (!lbDisplay.RLeftButton)
        {
            lbDisplay.RMouseX = lbDisplay.MMouseX;
            lbDisplay.RMouseY = lbDisplay.MMouseY;
            lbDisplay.RLeftButton = 1;
        }
    }
}

void set_right_button(bool down)
{
    if (down)
    {
        lbDisplay.MRightButton = 1;
        if (!lbDisplay.RightButton)
        {
            lbDisplay.MouseX = lbDisplay.MMouseX;
            lbDisplay.MouseY = lbDisplay.MMouseY;
            lbDisplay.RRightButton = 0;
            lbDisplay.RightButton = 1;
        }
    }
    else
    {
        lbDisplay.MRightButton = 0;
        if (!lbDisplay.RRightButton)
        {
            lbDisplay.RMouseX = lbDisplay.MMouseX;
            lbDisplay.RMouseY = lbDisplay.MMouseY;
            lbDisplay.RRightButton = 1;
        }
    }
}

bool panel_visible()
{
    return (game.operation_flags & GOF_ShowGui) != 0 && status_panel_width > 0
        && lbDisplay.GraphicsScreenWidth > 0 && lbDisplay.GraphicsScreenHeight > 0;
}

/** Moves the game's own cursor (not the Windows one) to screen position
 *  @p x, @p y, in the units GetMouseX() reports. */
void move_game_cursor(long x, long y)
{
    const long px = (pixel_size > 0) ? pixel_size : 1;
    LbMouseSetPositionInitial(x / px, y / px);
}

} // namespace

void ApplyPointer(const PointerInput& in, PointerResult* out)
{
    out->hit = false;
    MapHit hit;
    if (in.buttons_only)
    {
        // Possessing: the game keeps its cursor centred and turns the creature
        // from the headset; only the buttons are needed.
    }
    else if (in.valid && in.panel_hit && panel_visible())
    {
        // On the wrist panel: put the game's cursor on the same spot of the
        // real side panel, so its buttons see the clicks.
        move_game_cursor((long)(in.panel_u * status_panel_width),
                         (long)((1.0f - in.panel_v) * lbDisplay.GraphicsScreenHeight));
    }
    else if (in.valid && RaycastMap(in.ox, in.oy, in.oz, in.dx, in.dy, in.dz, &hit))
    {
        // On the dungeon: keep the game's cursor in the middle of the 3D view,
        // so clicks go to the world and not to whatever GUI the mouse is over.
        move_game_cursor(local_state.engine_window_x + local_state.engine_window_width / 2,
                         local_state.engine_window_y + local_state.engine_window_height / 2);

        out->hit = true;
        out->x = hit.x;
        out->y = hit.y;
        out->z = hit.z;

        // The input code only trusts the pointed-at state while the pointer is
        // inside the 3D view, so park it in the middle of the view.
        const int px = (pixel_size > 0) ? pixel_size : 1;
        pointer_x = local_state.engine_window_width / px / 2;
        pointer_y = local_state.engine_window_height / px / 2;
        const int32_t frac_x = (int32_t)hit.x & 0xFF;
        const int32_t frac_y = (int32_t)hit.y & 0xFF;
        floor_pointed_at_x = block_pointed_at_x = top_pointed_at_x = hit.stl_x;
        floor_pointed_at_y = block_pointed_at_y = top_pointed_at_y = hit.stl_y;
        pointed_at_frac_x = top_pointed_at_frac_x = frac_x;
        pointed_at_frac_y = top_pointed_at_frac_y = frac_y;
        me_pointed_at = get_map_block_at(hit.stl_x, hit.stl_y);
    }

    // Only button changes are passed on, so the real mouse keeps working too.
    const bool select = in.valid && in.select;
    const bool action = in.valid && in.action;
    if (select != s_left_down)
    {
        s_left_down = select;
        set_left_button(select);
    }
    if (action != s_right_down)
    {
        s_right_down = action;
        set_right_button(action);
    }
}

bool SpellChosen()
{
    struct PlayerInfo* player = get_my_player();
    if (player == nullptr)
        return false;
    const struct PlayerStateConfigStats* plrst = get_player_state_stats(player->work_state);
    return plrst != nullptr && plrst->pointer_group == PsPg_Spell;
}

bool PossessChosen()
{
    struct PlayerInfo* player = get_my_player();
    return player != nullptr && (player->work_state == PSt_CtrlDirect || player->work_state == PSt_FreeCtrlDirect);
}

void ReturnToHand()
{
    struct PlayerInfo* player = get_my_player();
    if (player != nullptr)
        set_players_packet_action(player, PckA_SetPlyrState, PSt_CtrlDungeon, 0, 0, 0);
}

bool MenuScreenWanted(bool in_level)
{
    return !in_level || a_menu_window_is_active();
}

void ApplyMenuTouch(const MenuInput& in)
{
    if (in.valid && lbDisplay.GraphicsScreenWidth > 0 && lbDisplay.GraphicsScreenHeight > 0)
    {
        move_game_cursor((long)(in.u * lbDisplay.GraphicsScreenWidth),
                         (long)((1.0f - in.v) * lbDisplay.GraphicsScreenHeight));
    }
    const bool press = in.valid && in.press;
    if (press != s_left_down)
    {
        s_left_down = press;
        set_left_button(press);
    }
}

void GetPanelInfo(PanelInfo* out)
{
    out->visible = panel_visible();
    out->width_frac = out->visible ? (float)status_panel_width / (float)lbDisplay.GraphicsScreenWidth : 0.0f;
}

} // namespace VR
