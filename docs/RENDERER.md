# Vulkan Renderer

The port renders through a from-scratch Vulkan render device, **`VulkanDrv`**, that implements
the build-219 `URenderDevice` interface. It replaces the legacy OpenGL ES device and is the
supported renderer. The design targets the rendering math of UE1's OpenGL driver as the 1:1
reference, so the look matches the original rather than a port-specific approximation.

The renderer reuses a vendored Vulkan support layer (`ZVulkan`: volk, VMA, builders, swapchain)
and a manager-based device architecture (command buffers, descriptor sets, render passes,
framebuffers, samplers, textures, uploads) adapted from the UT99 Vulkan device to the build-219
engine and to Android/NDK. Shaders are **precompiled to SPIR-V** and embedded — there is no
runtime GLSL compiler on device.

## What works

- **2D / HUD** — `DrawTile`, `Draw2DLine`, `Draw2DPoint` for the menu, HUD, and fonts. Eight
  blend/mask pipelines, palette (P8) and BGRA texture upload with caching, and per-frame
  host-visible vertex buffers. The viewport/scissor is set per frame from the engine's scene
  subregion.
- **3D world (BSP)** — `DrawComplexSurface` renders BSP surfaces as **base × lightmap × 2.0
  overbright**, matching KHG's baked lighting. Translucent / modulated / highlighted world
  pipelines are supported (only opaque surfaces write depth).
- **Actors / meshes / sprites** — `DrawGouraudPolygon` with a single texture plus per-vertex
  Gouraud color.
- **Reverse-Z depth** — a `D32_SFLOAT` depth attachment cleared to 0.0 with a
  `GREATER_OR_EQUAL` compare. This was the structural fix for an earlier "see-through surfaces"
  problem under the GLES path (low-precision depth losing coplanar BSP faces).
- **Engine-exact projection** — the projection matrix is built from the engine's own projection
  math (`ScreenX = FX2 + Point.X * ProjZ / Point.Z`), computed from the actor's stable
  `DesiredFOV` with a hor+ widescreen correction. It deliberately does **not** use the live
  `FOVAngle` (which the menu intro leaves mid-zoom, compressing the scene) nor the legacy GLES
  auto-FOV.
- **Sleep / resume** — on resume, the Vulkan surface is rebuilt from the current
  `ANativeWindow` and the swapchain is recreated; frames are skipped while backgrounded. The old
  surface is held alive until the old swapchain built on it is destroyed (avoiding a
  use-after-free), and the command pool is persistent across swapchain recreation. Suspend/resume
  is confirmed working on-device.
- **Android pre-rotation** — the swapchain forces an `IDENTITY` pre-transform where supported
  (the default rotated-90° transform otherwise rotated the HUD).

## Key fixes

- **Command-pool reset bit (the breakthrough).** The vendored Vulkan command pool was created
  with `flags = 0`. Because per-frame command buffers are re-recorded every frame via an
  implicit reset, this is undefined behaviour without
  `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`. The symptom was a few clean frames, then a
  GPU fault producing garbled streaks and a frozen render loop (the frame fence never signalled).
  This masqueraded as bugs in Gouraud meshes, dynamic textures, projection, and FMV — all of
  which were actually fine. Setting
  `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` made
  the whole 3D renderer stable.

- **Backdrop depth-write (FMV black).** A full-screen black backdrop drawn through the point
  pipeline was writing window-depth 0.0 across the screen with depth-write enabled, which then
  occluded the FMV tile under reverse-Z. Fix: the point pipeline now tests depth but does not
  write it, so the backdrop still covers the screen without blocking subsequent tiles.

- **Real-time texture resize / recreate.** Cached `VkImage`s for real-time texture slots (FMV
  content, splash, scripted textures) were created once and never recreated on a size change, so
  a larger frame copied into a smaller image produced a sheared/garbled image (e.g. a 640×480
  briefing uploaded into the image first sized by the 480×360 intro). Fix: when the
  size/format of a real-time slot changes, defer-delete the old image/view (safe against
  in-flight use) and rebuild at the new size.

- **Palette (P8) masked alpha.** KHG's build-219 palettes leave `FColor.A == 0` and derive mask
  alpha from the palette **index**, not from `palette[idx].A`. The P8 uploader (carried over from
  a 469-based driver that assumes `A == 255`) was therefore writing alpha 0 for every opaque
  pixel, so masked P8 textures — fonts and HUD glyphs — were entirely alpha-test-discarded
  ("menu but no text"). Fix: derive alpha from the index for masked textures and force opaque
  pixels to `A = 255`, matching the proven OpenGL ES reference. This also resolved a related
  "destroyed object shows a flat texture square" artifact, which was the same masked-alpha bug.

- **Single rendering context.** The GLES-backed `SDL_CreateRenderer` is no longer created for
  Vulkan windows; a second GL context on the shared `ANativeWindow` was corrupting Vulkan
  presentation ("renders for a second then garbles").

## Performance

On the development device the port is **vsync-bound at 60 fps** with substantial CPU headroom —
game logic is well under 1 ms per frame, and the "render + present" time is mostly the CPU
blocking on the present fence waiting for vsync, not GPU/CPU work. The only lever for higher
frame rates is uncapping vsync (the renderer has a vsync toggle that switches to triple
buffering), which is only worthwhile on panels above 60 Hz.

## Known remaining items

- Minor present-timing glitching/tearing in some conditions.
- A few effect meshes / real-time textures may not fully animate in every case and need
  per-case verification.
- Finishing the removal of the legacy OpenGL ES driver from the build.
- Development-time diagnostic logging is being removed before release.
