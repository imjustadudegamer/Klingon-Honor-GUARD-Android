/*=============================================================================
	FMVPlayer.h: Klingon Honor Guard FMV (Indeo 5 / IV50 AVI) playback for Android.

	KHG triggers cutscenes from UnrealScript via `ConsoleCommand("playavi <file> ...")`,
	originally handled by Windows VFW/DirectShow in the desktop build. We decode the user's own
	AVI on-device with FFmpeg (LGPL): Indeo 5 video + MS-ADPCM audio.

	To preserve KHG's presentation, the video is composited UNDER the live engine HUD
	(KlingonHUDIntroNull draws the Klingon border/logo/ESC overlay): the playavi handler
	drives a blocking loop that, each frame, advances decode (UE1FMVAdvance) and renders a
	normal engine frame (scene -> video -> HUD). UGameEngine::Draw calls UE1FMVDrawActiveFrame
	between the world and the HUD so the video appears beneath the overlay. Audio plays
	through an SDL audio device and is the master clock.
=============================================================================*/
#ifndef KHG_FMVPLAYER_H
#define KHG_FMVPLAYER_H

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

// Open a clip. Returns 1 on success (a clip is now active), 0 on failure (nothing active).
int  UE1FMVOpen( const char* FullPath, struct SDL_Window* Window );

// Advance decode toward the audio (master) clock: pump audio to the device and upload the
// video frame due now. Returns 1 while the clip is still playing, 0 when it has ended or the
// user skipped (any key / controller button / touch). Call once per rendered frame.
int  UE1FMVAdvance( void );

// Blit the current decoded video frame (full-screen, letterboxed) into the current GL
// framebuffer. Intended to be called from UGameEngine::Draw, after the world and before the
// HUD, so the engine's overlay composites on top. No-op if no clip is active. Uses the
// SDL window stored at UE1FMVOpen, so the Engine-side caller needs no SDL types.
void UE1FMVDrawActiveFrame( void );

// True (1) while a clip is open/active. Used by the Draw hook to decide whether to blit.
int  UE1FMVIsActive( void );

// Present a fixed BGRA image (top-down, W*H*4) as a timed, skippable full-screen still through the
// same path as the FMV. Drive with the playavi loop: while(UE1FMVAdvance()){ Engine->Draw(Viewport); }.
// Used for the retail MicroProse launcher splash shown after the intro movie.
int  UE1FMVShowStill( const unsigned char* Bgra, int W, int H, double Seconds );

// Stop and release the active clip.
void UE1FMVClose( void );

// Immediately clear the current GL window to black and present it (both swapchain buffers). Use
// before a slow UE1FMVOpen of a large file (e.g. the startup intro) so the previously drawn frame
// (the menu) isn't left on screen during the open. No-op if no GL window is current.
void UE1FMVBlackout( void );

// --- KHG decorated console-frame compositing (buffer/buildup/breakdn) -------------------
// The ornate Klingon comm-console frame is NOT in the content clip and NOT drawn by any KHG
// code: it is a separate video. `buildup.avi` assembles the frame, `breakdn.avi` removes it,
// and the content clip (char in a hex on a transparent grey/dark surround) is composited INTO
// the frame's hex window. The playavi `Y`/`C` flag triggers this. We reproduce it: play
// buildup.avi, retain its final frame as the persistent console overlay, then composite the
// content clip into the hex window, then play breakdn.avi.

// Snapshot the current decoded frame as the persistent console-frame overlay. Call right after
// buildup.avi finishes playing (before UE1FMVClose). Survives UE1FMVClose / clip changes.
void UE1FMVRetainAsOverlay( void );

// Get the retained console-frame overlay (BGRA8, top-down; alpha=255 ornate border, 0 hex window) for
// the Vulkan present path. Returns 1 if an overlay is retained. Draw it (masked) OVER the hex content.
int  UE1FMVGetOverlayBGRA( const unsigned char** outData, int* outW, int* outH );

// Enable/disable compositing the active clip INTO the retained overlay's hex window. When on (and
// an overlay is retained), UE1FMVDrawActiveFrame draws the console frame with the content clip
// masked into the hex window; otherwise the clip plays full-screen as usual.
void UE1FMVSetComposite( int On );

// Release the retained console-frame overlay and turn compositing off.
void UE1FMVReleaseOverlay( void );

// True (1) while the active clip is compositing into a retained console frame (the Y/C "framed"
// cutscenes). False for plain full-screen movies (intro.avi, new-game). The Draw hook uses this to
// decide Z-order: framed clips draw UNDER the HUD (the decorated frame/HUD overlays on top);
// full-screen movies draw ON TOP of the HUD/menu so nothing shows through.
int  UE1FMVIsComposite( void );

#ifdef __cplusplus
}
#endif

#endif // KHG_FMVPLAYER_H
