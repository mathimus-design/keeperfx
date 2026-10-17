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
#include "frontend.h"        // status_panel_width, menu_is_active(), active_buttons[]
#include "bflib_guibtns.h"   // struct GuiButton, LbBtnF_*
#include "gui_frontmenu.h"   // GMnu_*, get_active_menu(), menu_id_to_number()
#include "frontmenu_ingame_evnt.h" // my_visible_event_idx
#include "game_legacy.h"     // game.operation_flags, GOF_ShowGui
#include "config_players.h"  // get_player_state_stats(), PsPg_Spell, PSt_*
#include "packets.h"         // set_players_packet_action(), PckA_SetPlyrState
#include "bflib_keybrd.h"    // lbKeyOn[], KC_ESCAPE
#include "creature_battle.h" // active_battle_exists(), creature_battle_get(), step_battles_forward()
#include "map_events.h"      // struct Event, EvKind_*
#include "dungeon_data.h"    // get_my_dungeon(), visible_battles, event_button_index
#include "local_camera.h"    // move_local_camera_to_position()
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

int PlayerWorkState()
{
    // The state plus which room / power / trap / door is chosen, so switching
    // from one room (or spell...) to another counts as a change too.
    struct PlayerInfo* player = get_my_player();
    const struct UserState* ustate = get_local_user_state();
    if (player == nullptr || ustate == nullptr)
        return 0;
    uint32_t tool = player->work_state;
    tool = tool * 31u + ustate->chosen_room_kind;
    tool = tool * 31u + (uint32_t)ustate->chosen_power_kind;
    tool = tool * 31u + (uint32_t)ustate->chosen_trap_kind;
    tool = tool * 31u + (uint32_t)ustate->chosen_door_kind;
    return (int)tool;
}

bool PlainHand()
{
    struct PlayerInfo* player = get_my_player();
    return player != nullptr && player->work_state == PSt_CtrlDungeon;
}

bool FindLatestFight(float* x, float* y)
{
    struct PlayerInfo* player = get_my_player();
    struct Dungeon* dungeon = get_my_dungeon();
    if (player == nullptr || dungeon_invalid(dungeon))
        return false;
    bool found = false;
    if (active_battle_exists(player->id_number))
    {
        struct CreatureBattle* battle = creature_battle_get(dungeon->visible_battles[0]);
        struct Thing* thing = (battle != nullptr) ? thing_get(battle->first_creatr) : nullptr;
        if (thing_exists(thing))
        {
            *x = (float)thing->mappos.x.val;
            *y = (float)thing->mappos.y.val;
            found = true;
        }
        step_battles_forward(player->id_number);
    }
    if (!found)
    {
        // The newest alert button of a dangerous kind (later buttons are newer).
        for (int i = EVENT_BUTTONS_COUNT; i >= 0 && !found; i--)
        {
            const int idx = dungeon->event_button_index[i];
            if (idx <= 0 || idx >= EVENTS_COUNT)
                continue;
            const struct Event* event = &game.event[idx];
            if ((event->flags & EvF_Exists) == 0)
                continue;
            switch (event->kind)
            {
            case EvKind_HeartAttacked: case EvKind_EnemyFight: case EvKind_Breach:
            case EvKind_RoomUnderAttack: case EvKind_AlarmTriggered: case EvKind_FriendlyFight:
            case EvKind_EnemyDoor: case EvKind_RoomTakenOver: case EvKind_RoomLost:
                if (event->mappos_x > 0 || event->mappos_y > 0)
                {
                    *x = (float)event->mappos_x;
                    *y = (float)event->mappos_y;
                    found = true;
                }
                break;
            default:
                break;
            }
        }
    }
    if (!found && (dungeon->last_combat_location.x.val > 0 || dungeon->last_combat_location.y.val > 0))
    {
        *x = (float)dungeon->last_combat_location.x.val;
        *y = (float)dungeon->last_combat_location.y.val;
        found = true;
    }
    if (found)
        move_local_camera_to_position((MapCoord)*x, (MapCoord)*y);  // the PC view follows
    return found;
}

bool InFrontendMenus()
{
    return frontend_menu_state != FeSt_INITIAL;
}

void SetEscapeKey(bool down)
{
    lbKeyOn[KC_ESCAPE] = down ? 1 : 0;
}

bool LevelDecided()
{
    struct PlayerInfo* player = get_my_player();
    return player != nullptr && player->victory_state != VicS_Undecided;
}

bool MenuScreenWanted(bool in_level)
{
    // Paused too: the screen shows the game's pause display, and its menus
    // (options, quit) can be reached from it. Not for a won or lost level: the
    // table stays, to look around, until the green "next" button is tapped.
    return !in_level || a_menu_window_is_active() || (game.operation_flags & GOF_Paused) != 0;
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

void GetInfoBox(InfoBoxInfo* out)
{
    *out = InfoBoxInfo();
    if ((game.operation_flags & GOF_ShowGui) == 0 || lbDisplay.GraphicsScreenWidth <= 0 || lbDisplay.GraphicsScreenHeight <= 0)
        return;
    static const short boxes[] = { GMnu_TEXT_INFO, GMnu_BATTLE, GMnu_DUNGEON_SPECIAL, GMnu_SPELL_LOST };
    for (short id : boxes)
    {
        if (!menu_is_active(id))
            continue;
        const struct GuiMenu* gmnu = get_active_menu(menu_id_to_number(id));
        if (gmnu == nullptr || gmnu->width <= 0 || gmnu->height <= 0)
            continue;
        const float sw = (float)lbDisplay.GraphicsScreenWidth, sh = (float)lbDisplay.GraphicsScreenHeight;
        out->visible = true;
        out->x0 = (float)gmnu->pos_x / sw;
        out->y0 = (float)gmnu->pos_y / sh;
        out->x1 = (float)(gmnu->pos_x + gmnu->width) / sw;
        out->y1 = (float)(gmnu->pos_y + gmnu->height) / sh;
        // Which message: the event the text box shows, told apart per box kind.
        out->message_id = (int)id * 1000 + (int)my_visible_event_idx;
        // Its small clickable buttons, left to right as the game lays them out;
        // big ones (the text area itself) are skipped.
        const int menu_num = menu_id_to_number(id);
        for (int i = 0; i < ACTIVE_BUTTONS_COUNT && out->button_count < 6; i++)
        {
            const struct GuiButton* gbtn = &active_buttons[i];
            if ((gbtn->flags & (LbBtnF_Active | LbBtnF_Visible)) != (LbBtnF_Active | LbBtnF_Visible)
                || gbtn->gmenu_idx != menu_num || gbtn->click_event == nullptr)
                continue;
            if (gbtn->width <= 0 || gbtn->height <= 0 || gbtn->width > gmnu->width / 3 || gbtn->height > gmnu->height / 2)
                continue;
            InfoBoxInfo::Button& b = out->buttons[out->button_count++];
            b.x0 = (float)gbtn->scr_pos_x / sw;
            b.y0 = (float)gbtn->scr_pos_y / sh;
            b.x1 = (float)(gbtn->scr_pos_x + gbtn->width) / sw;
            b.y1 = (float)(gbtn->scr_pos_y + gbtn->height) / sh;
        }
        return;
    }
}

void ApplyInfoTouch(const MenuInput& in, const InfoBoxInfo& box)
{
    if (in.valid && box.visible)
    {
        const float x = box.x0 + in.u * (box.x1 - box.x0);
        const float y = box.y0 + (1.0f - in.v) * (box.y1 - box.y0);
        move_game_cursor((long)(x * lbDisplay.GraphicsScreenWidth), (long)(y * lbDisplay.GraphicsScreenHeight));
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
