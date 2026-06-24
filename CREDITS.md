# Credits & Acknowledgements

This Android port stands on a large body of prior work. Every component below is gratefully
credited to its authors. Nothing here is claimed as original except the Android port glue, the
build-219 engine-compatibility work, the Vulkan render device, and the handheld-specific fixes
noted at the bottom. **No retail game assets are distributed in this repository.**

## The game

- **Star Trek: Klingon Honor Guard** (1998) — developed and published by **MicroProse**, built on
  the Unreal Engine. *Star Trek* and all related marks are the property of their respective
  rights holders (Paramount / CBS), and the game itself is owned by its current rights holders.
  This is a non-commercial fan port; you must supply your own legally-owned copy of the game data.

## Engine

- **Unreal Engine 1** — © **Epic Games** (Epic MegaGames). Unreal® and the Unreal Engine are
  trademarks of Epic Games, Inc. This port uses a publicly-available UE1 v200 source baseline with
  proprietary assets removed, adapted to be binary-compatible with KHG's build-219 engine fork.
- **[fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1)** — the UE1 v200 source-port baseline this
  project builds on.
- **Andiweli/Unreal-Android** — the Android packaging / bring-up baseline (Activity bridge,
  Gradle/CMake layout) this port derives from.

## Renderer

- The **Vulkan render device** (`VulkanDrv`) was written for this port against the build-219
  `URenderDevice` interface. Its architecture is informed by the **UT99 Vulkan render device** (a
  Vulkan driver for Unreal Tournament '99).
- **ZVulkan**, **volk**, and **Vulkan Memory Allocator (VMA)** — the Vulkan loader and
  memory-allocation helpers used by the renderer (MIT; see source headers).
- Android-Vulkan plumbing patterns were cross-referenced against the **Quake3e** / Elite Force
  Vulkan port lineage.

## Bundled libraries

- **SDL2** — Sam Lantinga and the SDL contributors (zlib license). Android Activity bridge,
  windowing, and controller input. <https://libsdl.org>
- **OpenAL Soft** — the OpenAL Soft contributors (LGPL-2.0-or-later). Audio backend.
- **libxmp** — tracker/module music decoding (UMX), under its bundled license.
- **FFmpeg** — the FFmpeg team (LGPL-2.1-or-later). Cutscene (FMV / AVI) video and audio decoding;
  configured as an LGPL build and dynamically linked. <https://ffmpeg.org>

See **[THIRDPARTY.md](THIRDPARTY.md)** for versions and the authoritative license notices.

## Not included (proprietary — supply your own)

- **Retail Klingon Honor Guard game data** (`System`, `Maps`, `Textures`, `Sounds`, `Music`, and
  the `.avi` movies) — not redistributed. You must provide your own legally-owned copy.

## This port

The Android port and handheld integration — the build-219 engine-compatibility layer, the Vulkan
render device, the on-device FMV/cutscene system, the Android save/load path handling, controller
configuration, handheld UI scaling, and the Android lifecycle (sleep/resume) handling — were
contributed by this project's authors and contributors.

---

If any attribution here is incomplete or incorrect, please open an issue and it will be fixed.
</content>
