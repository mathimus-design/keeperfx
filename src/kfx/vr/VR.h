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

#ifdef __cplusplus
}
#endif

#endif // KFX_VR_VR_H
