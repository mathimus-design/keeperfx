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

} // namespace VR
