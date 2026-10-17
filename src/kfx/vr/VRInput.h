/******************************************************************************/
// Dungeon Keeper - VR mode
/******************************************************************************/
/** @file VRInput.h
 *     The right controller as the Keeper's hand.
 * @par Purpose:
 *     The controller's pointing ray stands in for the mouse cursor: where it
 *     hits the dungeon is written into the same "pointed at" globals the
 *     desktop renderer fills from the mouse, and the trigger / A button are
 *     fed in as left / right mouse button presses. Everything after that --
 *     picking up, dropping, slapping, tagging for digging, possessing -- is
 *     the game's own input handling, so it follows all the normal rules.
 */
/******************************************************************************/
#pragma once

namespace VR {

/** Render thread -> game thread. */
struct PointerInput {
    bool valid = false;              /**< Controller tracked and a level is shown. */
    float ox = 0, oy = 0, oz = 0;    /**< Ray origin, game units. */
    float dx = 0, dy = 0, dz = 0;    /**< Ray direction, game units. */
    bool select = false;             /**< Held: acts as the left mouse button. */
    bool action = false;             /**< Held: acts as the right mouse button. */
    bool buttons_only = false;       /**< Possessing: pass the buttons on, but there is no ray. */
    bool panel_hit = false;          /**< The ray is on the wrist panel instead of the dungeon. */
    float panel_u = 0, panel_v = 0;  /**< Where on the panel: 0..1 left to right, bottom to top. */
};

/** Game thread -> render thread. */
struct PointerResult {
    bool hit = false;
    float x = 0, y = 0, z = 0;       /**< Where the ray meets the dungeon, game units. */
};

/** Game thread -> render thread: the desktop side panel, which the wrist
 *  panel shows a copy of. */
struct PanelInfo {
    bool visible = false;
    float width_frac = 0;            /**< Panel width as a fraction of the screen width. */
};

/** Game thread, after the frame is rendered: point the game's cursor state at
 *  the controller's target and pass on button changes. */
void ApplyPointer(const PointerInput& in, PointerResult* out);

/** Game thread: where the side panel is on screen. */
void GetPanelInfo(PanelInfo* out);

/** Render thread -> game thread: a finger (or the ray) on the floating menu
 *  screen, a copy of the whole desktop screen shown while a menu is up. */
struct MenuInput {
    bool valid = false;              /**< Over the screen. */
    float u = 0, v = 0;              /**< Where: 0..1 left to right, bottom to top. */
    bool press = false;              /**< Touching / trigger held: the left mouse button. */
};

/** Game thread: true while a spell is chosen and waiting to be cast (the
 *  player state's cursor is the spell one); the left hand casts it then. */
bool SpellChosen();

/** Game thread: true while Possess is chosen and waiting for a creature. */
bool PossessChosen();

/** Game thread: back to the plain Keeper's hand, as the panel's hand button
 *  does (drops a chosen spell). */
void ReturnToHand();

/** Game thread: a value for the chosen tool (player state plus the chosen
 *  room / spell / trap / door), which changes whenever one is picked; and
 *  whether the player has the plain hand. */
int PlayerWorkState();
bool PlainHand();

/** Game thread: true once the level is won or lost (waiting to continue). */
bool LevelDecided();
/** Where to look for trouble: the next of your battles (each call steps on
 *  to the next one, as the game's "zoom to fight" key does), else the most
 *  recent attack / breach / intruder alert, else the last fight seen.
 *  Game units; false when there is nothing. Also moves the PC camera there. */
bool FindLatestFight(float* x, float* y);
/** The game is in its own menus (main menu, level map, level stats...), not a level. */
bool InFrontendMenus();
/** Holds Escape down (or lets it go) for the frontend's back handling. */
void SetEscapeKey(bool down);

/** Game thread: true while the game shows a menu rather than the dungeon:
 *  the frontend (main menu, campaign map, ...), a menu window in a level, or
 *  the game paused. */
bool MenuScreenWanted(bool in_level);

/** Game thread: moves the game's cursor to the touched spot and presses the
 *  left button while touching. */
void ApplyMenuTouch(const MenuInput& in);

/** Game thread -> render thread: the message box (objective / info text,
 *  battle and special boxes) the desktop shows over the dungeon view. */
struct InfoBoxInfo {
    bool visible = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   /**< Screen fractions, y from the top. */
    int message_id = 0;                      /**< Changes when a different message is shown. */
    /** The box's own small buttons (close tick, zoom-to eye, scroll), which
     *  VR repeats in a row under the box where they are easier to reach. */
    struct Button { float x0, y0, x1, y1; } buttons[6];
    int button_count = 0;
};

/** Game thread: where the message box is, if one is open. */
void GetInfoBox(InfoBoxInfo* out);

/** Game thread: a touch on the message box copy; u/v are within the box (they
 *  may fall outside 0..1 for a button the game draws outside it). */
void ApplyInfoTouch(const MenuInput& in, const InfoBoxInfo& box);

} // namespace VR
