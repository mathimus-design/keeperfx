/******************************************************************************/
// Dungeon Keeper - VR mode
/******************************************************************************/
/** @file VRSystem.h
 *     OpenXR session and the VR tabletop pass, driven by RendererOpenGL.
 * @par Threading:
 *     GameThreadSync() runs on the game thread inside PresentFrame(), while the
 *     render thread is idle, so it can hand data over without locking.
 *     Everything else runs on the render thread, which owns the GL context.
 */
/******************************************************************************/
#pragma once

class GLWorldViewRenderer;

namespace VR {

/** Game thread, render thread idle: snapshot the map for the next VR frame. */
void GameThreadSync();

/** Render thread, after the desktop frame is drawn: render and submit both eyes.
 *  @p screen_fbo holds that frame (screen_w x screen_h); the side panel is copied
 *  from it onto the wrist panel. The window (window_w x window_h) then gets
 *  a copy of the left eye, so people at the PC can watch.
 *  Starts the OpenXR session on first use. */
void RenderThreadFrame(GLWorldViewRenderer& world, unsigned int screen_fbo, int screen_w, int screen_h,
                       int window_w, int window_h);

/** Render thread: true while a headset session is running (desktop vsync is
 *  turned off then, so it does not hold back the headset's frame timing). */
bool IsSessionRunning();

/** Render thread: tear down the session and GL objects. */
void RenderThreadShutdown();

} // namespace VR
