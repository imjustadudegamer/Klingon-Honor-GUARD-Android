# FMV / Cutscene System

*Klingon Honor Guard* plays its cutscenes — the intro, mission briefings, and in-game Klingon
"comm console" cutscenes — as AVI movies. On the original Windows release these were decoded by
Indeo 5 + DirectShow inside the game's Windows video code, which does not exist on Android (and
no longer works on modern Windows either). This port decodes the movies on-device with FFmpeg
and presents them through the render device.

## Decoder

The movies are **Indeo 5** (`IV50`) video with **MS-ADPCM** (`adpcm_ms`, 22050 Hz stereo) audio
in an AVI container. They are decoded by a **minimal, LGPL-only FFmpeg** build for armeabi-v7a:

- Components enabled: the AVI demuxer, the Indeo 5 decoder, the MS-ADPCM decoder, swscale, and
  the file protocol — nothing else.
- Built **without** `--enable-gpl` and **without** `--enable-nonfree`, so the engine takes on no
  GPL copyleft. Obligation: include the FFmpeg license text and attribution; the project ships
  the *decoder*, never the user's video.

Playback is **audio-master synced** (the audio clock drives frame scheduling, the standard
ffplay/ScummVM model). The player exposes the current decoded frame as a BGRA buffer, which the
engine wraps as a transient texture and draws through the render device — so presentation is
**device-agnostic** (the same path works under Vulkan and OpenGL ES) rather than tied to a GL
blit.

## The `playavi` handler

KHG's UnrealScript triggers movies with `ConsoleCommand("playavi <file>.avi <tokens...>")`. A
native `playavi` Exec handler in the SDL viewport parses the filename and the trailing flag
tokens, pauses the game, runs the decode/present loop, allows skipping (any button / touch /
key), then resumes. The flag tokens select playback mode, including whether a clip is a plain
fullscreen movie or part of the decorated comm-frame chain.

Fullscreen clips (the intro, ship cinematics, briefings on the plain path) are letterboxed to
their native aspect and centered. The intro movie and the MicroProse launcher splash are played
once at startup, before the menu, and are skippable (and no-op if the files are absent).

## The decorated Klingon comm-frame

The in-game comm/briefing cutscenes are the interesting case. The character video itself is just
**a figure composited into a hexagonal window on a flat dark surround** — there is **no ornate
frame baked into the clip**. The decorated Klingon console frame (riveted bronze/red border,
rank/emblem corners, gold-edged central hex window) is a **separate video chain**, not in the
clip and not drawn by any of the original game's code in a way the port could reuse.

The chain is three movies driven by the `playavi` flags:

| Clip | Role |
|---|---|
| `buildup.avi` | assembles the ornate console frame |
| *(content clip)* | the character video composited into the frame's hex window |
| `breakdn.avi`   | tears the frame back down |

The port reproduces this by:

1. Playing `buildup.avi` and **retaining its final frame** as an overlay (the fully-assembled
   ornate frame with its central hexagonal window).
2. Compositing the content clip into the hex window, then
3. Playing `breakdn.avi`.

### Compositing approach

The central window in the assembled frame is an irregular flat-topped hexagon (widest below
center), bounded by a bright gold ring. Two early approaches were rejected:

- An analytic SDF hexagon mask — the window is irregular, so guessed constants never fit.
- Color-keying the content clip's dark surround — fails, because the character's black hair is
  as dark as the surround, so distance-keying punches holes in the character.

The working method **masks the window, not the character**: the overlay (buildup's last frame)
has its central black window flood-filled (bounded by the bright gold ring) and baked into the
overlay texture's **alpha channel** (0 = window, 255 = border), with a measured-hexagon polygon
fallback if the flood-fill result is implausible. Compositing is then a trivial
`mix(content, frame, frame.alpha)`. Because the frame and content are pre-aligned at the same
resolution, this reproduces the exact window shape with no guessing and preserves the character's
dark hair. The decorated frame composites correctly on-device.

## Known remaining items

- Audio sync/skip handling across the `buildup → content → breakdn` clip transitions can still
  be refined.
- Fine centering of the content inside the hex window is being re-verified.
