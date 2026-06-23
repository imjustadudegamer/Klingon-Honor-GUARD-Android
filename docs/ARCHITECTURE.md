# Architecture

## Engine base

The port is built on the **[fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1)** /
**Andiweli/Unreal-Android** source port of **Unreal Engine 1 (v200)**. *Klingon Honor Guard*
runs on a later **build-219** fork of UE1.

A key fact shaped the whole effort: **KHG adds no native C++ package of its own.** Its game
package (`Klingons`) is entirely UnrealScript (~400 classes), and its native dependency is just
the stock UE1 native API as modified by the build-219 fork. So the real work is making the
v200-derived `Core`/`Engine` **binary-compatible with build 219**:

- **Native function indices** — the byte in compiled UnrealScript bytecode must dispatch to the
  same native function KHG was compiled against.
- **Class property layouts & serialization** — `UObject`/`AActor`/`APawn`/… property order and
  `Serialize()` behaviour must match build 219, or serialized defaults deserialize at the wrong
  offsets (the classic "Core can't load Klingons.u" failure).

The native build-219 API is close to the Android base (a delta of a few dozen native
intrinsics), and the missing pieces are recoverable from later public UE1 source.

## The native surface (platform layer)

Rendering, windowing, input, audio, and video are provided by native code on top of SDL2:

- **SDL2** — Android activity bridge, windowing, surface/lifecycle, controller input.
- **OpenAL Soft** — audio backend.
- **FFmpeg** (LGPL) — on-device FMV (movie) decoding.

The app's Android entry point is `MainActivity` in package `com.khg.android`.

## Source layout

All engine and native source lives under `app/src/main/cpp/`:

```
app/src/main/cpp/
├── UE1/Source/            # the Unreal Engine 1 C++ engine + drivers
│   ├── Core/              # object system, name table, script VM, serialization
│   ├── Engine/            # game engine: actors, physics, canvas, game loop, FMV hook
│   ├── Render/            # software/abstract render module (BSP, meshes)
│   ├── NSDLDrv/           # SDL2 platform layer: viewport, input, window, FMV player
│   ├── VulkanDrv/         # the Vulkan render device (the supported renderer)
│   ├── ZVulkan/           # vendored Vulkan helpers (volk, VMA, builders, swapchain)
│   ├── NOpenGLESDrv/      # legacy OpenGL ES render device (being retired)
│   ├── NOpenALDrv/        # OpenAL audio device
│   ├── Unreal/            # launcher / app bootstrap (SDLLaunch)
│   └── ...                # Fire, IpDrv, Editor, etc.
└── thirdparty/            # SDL2, openal-soft, ffmpeg, libxmp
```

- The **engine** (`UE1/Source/Core`, `Engine`, `Render`) is the single source of truth for all
  port modifications — these are edited in place.
- **`NSDLDrv`** is the SDL platform layer (viewport, controller input, window creation, and the
  FMV player).
- **`VulkanDrv`** is the renderer (see [RENDERER.md](RENDERER.md)).
- **`thirdparty/`** holds the vendored/fetched third-party libraries.

## Engine modifications for the port

The following changes were made to bring KHG up on the Android engine. They are grouped by area.

### Package loading & the script VM (build-219 compatibility)

- Added the `UStrProperty` native property class needed to load KHG's packages.
- Added missing native bytecode intrinsics, including the `>>>` operator
  (`execGreaterGreaterGreater_IntInt`), `execLogStr`, and `Canvas.StrLen`.
- Added a safety net for missing-named natives, and an intrinsic-class-bind fallback to script
  (so e.g. `Engine.Light` resolves correctly).
- Corrected a property-tag size class in serialization (a 16-bit size field that truncated at
  65536 and corrupted saves).

### De-CD (removing CD copy protection)

- KHG's copy-protection console-command check is satisfied natively (the `prsq` command result
  is mapped to the expected value) so the game passes the CD check without media present.
- *(Known gap: some in-game/boss music originally streamed from CD audio tracks; with no CD on
  Android those are silent unless redirected to packaged UMX.)*

### Save / load

- Replaced Windows-style backslash path construction in the save/load code with the engine's
  portable path separators, so save and hub files resolve correctly on Android's filesystem.
- Fixed a load failure where the engine's URL parser rejected save paths containing `/`
  (it treats `/` as a URL delimiter): the load and hub-pop sites now parse a slash-free basename
  and then point the URL's map field at the full qualified path directly.

### FMV (full-motion video)

- A native `playavi` Exec handler in the SDL viewport drives movie playback (the original
  handler lived in Windows VFW code that does not exist on Android).
- A new FFmpeg-based FMV player decodes the user's original AVIs on-device.
- An engine draw hook composites the decorated Klingon comm-frame around comm/briefing clips.
- See [FMV.md](FMV.md) for the full system.

### Input

- The SDL viewport maps the gamepad to a console-FPS-style layout; right-stick look and stick
  sensitivity are ini-tunable with a low-latency look filter. See [CONTROLS.md](CONTROLS.md).

### Renderer

- A from-scratch Vulkan render device (`VulkanDrv`) implementing the build-219 `URenderDevice`
  interface, with reverse-Z depth and engine-exact projection. See [RENDERER.md](RENDERER.md).
- Earlier OpenGL ES fidelity fixes (24-bit depth request, texture clamp/filter handling, shader
  precision) were made before the Vulkan device became the primary path.

### Engine robustness

The modified engine occasionally trips stock UE1 integrity assertions that the retail game
rarely hit (actor-lifecycle, collision-hash, and light-cache edge cases during heavy combat with
many dynamic lights and spawning/destroying debris). These are render-device-independent
engine-layer issues; they are handled by guarding null/stale references and recovering instead
of aborting, for playability. A latent-error audit pass hardened a number of additional
out-of-bounds and use-after-free paths across the object, render, mesh, and platform layers.

> One known deeper item, intentionally deferred: the global name-table can grow unbounded across
> very long sessions (transient objects), which can stress the 16-bit `FName` index family. The
> save-corruption symptoms are mitigated, but the durable root-cause fix is still open.
