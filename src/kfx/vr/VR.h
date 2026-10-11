/******************************************************************************/
// Dungeon Keeper - VR mode
/******************************************************************************/
/** @file VR.h
 *     C-visible switch for VR mode.
 * @par Purpose:
 *     VR mode shows the whole dungeon as a 3D tabletop in an OpenXR headset,
 *     alongside the normal desktop view. It is requested with the -vr command
 *     line switch, needs the OpenGL renderer, and is only compiled in when the
 *     build found the OpenXR loader (KFX_VR).
 */
/******************************************************************************/
#ifndef KFX_VR_VR_H
#define KFX_VR_VR_H

#ifdef __cplusplus
extern "C" {
#endif

/** Non-zero when the -vr command line switch was given. */
extern int vr_requested;

/** Game thread: while the headset shows the dungeon, the sound listener is
 *  where the headset looks at the table, facing the way the head faces
 *  (angle in game units, as LbArcTanAngle() returns). Returns 0 when the
 *  desktop camera should be used instead. */
int VR_GetSoundListener(int* x, int* y, int* z, int* angle);

/** Game thread: non-zero while the headset is showing the dungeon (and VR
 *  steers the game's cursor). */
int VR_IsHeadsetActive(void);

/** Game thread, while possessing a creature: the facing the headset asks for
 *  (yaw and pitch in game angle units, as the first-person packet carries
 *  them) and the left thumbstick (-100..100, x right, y forward). Returns 0
 *  when VR is not driving the possessed creature. */
int VR_GetPossessionControls(int* yaw, int* pitch, int* move_x, int* move_y);

/** Game thread: non-zero once after the controller's pause button was
 *  pressed; the input code then toggles pause as for the pause key. */
int VR_ConsumePauseRequest(void);

/** Game thread: non-zero while a VR pause press waits for the input code
 *  (without taking it), so the input code reads it even when a menu has the pointer. */
int VR_PauseRequestPending(void);

/** Game thread: once a swat of the Keeper's hand has picked a creature, its
 *  thing index (once), for the input code to slap like a right-click. */
int VR_ConsumeSlapTarget(void);

/** Game thread: non-zero once after A or the trigger was pressed while the
 *  level is won or lost; the input code then finishes the level as for the
 *  finish-level key (space). */
int VR_ConsumeFinishRequest(void);

/** Game thread: non-zero once after A was pressed on a menu screen; the high
 *  score table then takes it as Enter, to confirm the name being typed. */
int VR_ConsumeEnterRequest(void);

#ifdef __cplusplus
}
#endif

#endif // KFX_VR_VR_H
