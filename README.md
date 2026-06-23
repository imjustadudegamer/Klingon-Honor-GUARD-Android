# Klingon Honor Guard — Android

An unofficial fan port of the 1998 first-person shooter **Star Trek: Klingon Honor Guard**
(Unreal Engine 1) to Android.

This repository contains the **engine, platform layer, and renderer** only. It ships **no game
assets** — you must own a retail copy of *Klingon Honor Guard* and supply its data files
yourself (see [Requirements](#requirements--bring-your-own-game-data)).

---

## Project status

The port **boots and plays on-device** (developed and tested on an armeabi-v7a Snapdragon
handheld). What works today:

- **Boots to the menu and into gameplay** — engine init → main menu (ornate Klingon border,
  UMX menu music) → New Game → first mission loads and is playable, including combat.
- **Vulkan renderer working** — a from-scratch `VulkanDrv` render device renders the 3D BSP
  world with baked lightmaps, characters/meshes, translucency, and the HUD on top at ~60 fps.
  It uses reverse-Z depth and engine-exact projection. The older OpenGL ES path is being retired.
- **FMV cutscenes** — the intro movie, the MicroProse splash, mission briefings, and the
  decorated Klingon comm-frame cutscenes play back via an on-device FFmpeg decoder.
- **Save / load** — saving and loading game state works through the engine's hub/save system.
- **Controller support** — a console-FPS-style gamepad layout with ini-tunable stick
  sensitivity and a low-latency look filter.
- **Sleep / resume** — the Vulkan surface and swapchain are rebuilt correctly on
  background → foreground.

This is a work in progress. Some effects, fine FMV compositing details, and long-session
engine-stability edge cases are still being refined.

## How it works

The port is built on the **[fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1)** /
**Andiweli/Unreal-Android** Unreal Engine 1 source baseline (UE1 v200), surgically adapted up
to be binary-compatible with **KHG's build-219 fork** of the engine — KHG ships no native code
of its own, so the work is matching native-function indices and class serialization/layout to
build 219 rather than reconstructing a game DLL. The platform layer is SDL2-based; rendering is
done by a new Vulkan device written against the build-219 `URenderDevice` interface. The current
build targets **armeabi-v7a** (32-bit ARM).

## Requirements — Bring Your Own Game Data

**This repository contains no copyrighted game content.** To play, you need a retail
installation of *Star Trek: Klingon Honor Guard* and you must copy its data onto the device.

1. Install (or copy) your retail KHG so you have its `System`, `Maps`, `Textures`, `Sounds`,
   `Music`, and the `.avi` movie files from `System/`.
2. Place this data on the device under **`/sdcard/Unreal`**, preserving the original
   subdirectory layout (`/sdcard/Unreal/System`, `/sdcard/Unreal/Maps`, etc.). Movies (`.avi`)
   go in `/sdcard/Unreal/System`; UMX music goes in `/sdcard/Unreal/Music`.
3. Saves are written under `/sdcard/Unreal/Save`.

Full deploy and data-staging steps are in **[docs/BUILD.md](docs/BUILD.md)**.

## Quick build

```bash
./gradlew :app:assembleDebug
```

The APK lands at `app/build/outputs/apk/debug/app-debug.apk`. Prerequisites, the SDK/NDK
versions, and how to deploy and run on a device are in **[docs/BUILD.md](docs/BUILD.md)**.

## Features

- Vulkan render device (3D world + lightmaps, Gouraud meshes/sprites, 2D HUD/menu, reverse-Z).
- On-device FMV playback (intro, splash, briefings, decorated comm-frame compositing) via LGPL FFmpeg.
- Console-FPS gamepad layout, ini-tunable, with a low-latency right-stick look filter.
- Scalable 2D UI / fonts for handheld screens.
- Save / load and hub progression.
- Android lifecycle handling (sleep/resume surface recreation).

## Legal & Attribution

This is an **unofficial fan project** and is **not affiliated with or endorsed by** Epic Games,
MicroProse, Microsoft, or any rights holder.

- **Unreal Engine** © Epic Games. The engine baseline is derived from publicly available UE1
  v200 source with proprietary assets removed; see upstream for its status.
- **Star Trek: Klingon Honor Guard** © MicroProse / Microsoft and the respective rights holders.
  No game assets are included in this repository — you supply your own retail copy.
- **FFmpeg** is used for movie decoding under the **LGPL-2.1** (built without `--enable-gpl` /
  `--enable-nonfree`; Indeo 5 + AVI demuxer + MS-ADPCM + swscale only). See its license text.
- Built on **[Andiweli/Unreal-Android](https://github.com/fgsfdsfgs/UE1)** and the
  **[fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1)** source port.

See [THIRDPARTY.md](THIRDPARTY.md) for the full third-party notices.

## Credits

- The **fgsfdsfgs/UE1** and **Andiweli/Unreal-Android** projects for the UE1 source port and
  Android engine baseline.
- **SDL2**, **OpenAL Soft**, and **FFmpeg** for platform, audio, and video.
- Vulkan renderer architecture informed by the UT99 Vulkan render device.

## Documentation

- **[docs/BUILD.md](docs/BUILD.md)** — prerequisites, building, deploying, running.
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — engine base, source layout, and the
  engine modifications made for the port.
- **[docs/RENDERER.md](docs/RENDERER.md)** — the Vulkan render device.
- **[docs/FMV.md](docs/FMV.md)** — the cutscene / FMV system.
- **[docs/CONTROLS.md](docs/CONTROLS.md)** — controller configuration reference.
