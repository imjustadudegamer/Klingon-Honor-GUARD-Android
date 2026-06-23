# Third-Party Notices

This project bundles or links several third-party components. Their sources are
vendored under `app/src/main/cpp/` and built by CMake as part of the app. No
game assets are included (see the README).

## Engine base

- **fgsfdsfgs/UE1** — Unreal Engine 1 (v200) source-port baseline. The upstream
  describes itself as a UE1 v200 source tree with proprietary game assets
  removed; it is not affiliated with or endorsed by Epic Games. The Android and
  game-compatibility patches in this repo are applied directly to the vendored
  copy under `app/src/main/cpp/UE1/Source`.
- **Andiweli/Unreal-Android** — Android packaging/bring-up baseline this port
  derives from (Activity bridge, Gradle/CMake layout).

Unreal® and the Unreal Engine are trademarks of Epic Games, Inc.

## Libraries (vendored under `app/src/main/cpp/thirdparty/`)

| Component | Version | Purpose | License |
|---|---|---|---|
| SDL2 | 2.32.10 | Android Activity bridge, windowing, GL ES context, controller input | zlib |
| OpenAL Soft | 1.25.1 | OpenAL-compatible audio backend | LGPL-2.0-or-later |
| libxmp | bundled | Tracker/module music decoding (UMX) | see bundled `libxmp` license |
| FFmpeg | LGPL build | Cutscene (FMV/AVI) video + audio decoding | LGPL-2.1-or-later |
| ZVulkan / volk / VMA | bundled | Vulkan loader + memory allocation helpers (renderer) | MIT (see source headers) |
| glad_es | bundled | GL ES loader (legacy GLES path) | MIT / public-domain generator |

Refer to each component's upstream project and the license headers in its
vendored source for the authoritative terms.

## FFmpeg (LGPL) note

FFmpeg is used under the LGPL-2.1-or-later. It is configured as an LGPL build and
is **dynamically linked**: the prebuilt shared libraries ship in
`app/src/main/jniLibs/armeabi-v7a/` (packaged into the APK) and the matching
import stubs/headers live under `app/src/main/cpp/thirdparty/ffmpeg/`. Because the
libraries are dynamically linked, they can be replaced/relinked independently, as
the LGPL requires. FFmpeg source is available at <https://ffmpeg.org>.

## Build tooling

- Android Gradle Plugin `8.13.2`, Gradle `8.13`.
