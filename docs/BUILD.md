# Building & Running

This project builds an Android APK from an Unreal Engine 1 C++ engine plus native platform,
renderer, audio, and video code. The build is driven by Gradle and CMake/NDK.

## Prerequisites

Install an Android SDK with the following components (the exact versions this project is built
against):

| Component | Version |
|---|---|
| Android SDK platform | `android-36` |
| Build-tools | `36.0.0` |
| NDK | `27.0.12077973` |
| CMake | `3.22.1` |
| JDK | `21` |
| Gradle | `8.13` |

You can install the SDK components from the command line:

```bash
sdkmanager "platforms;android-36" "build-tools;36.0.0" "ndk;27.0.12077973" "cmake;3.22.1"
```

You also need `adb` and an **armeabi-v7a-capable** Android device (the current build targets
32-bit ARM only; the arm64 variant is not enabled).

## Configure the SDK location

Create a `local.properties` file in the project root pointing at your SDK:

```properties
sdk.dir=/path/to/Android/sdk
```

(Alternatively, export `ANDROID_HOME` / `ANDROID_SDK_ROOT`.)

## Build

From the project root:

```bash
./gradlew :app:assembleDebug
```

The build compiles the full native engine (the first clean build is slow; incremental rebuilds
are fast). The resulting APK is written to:

```
app/build/outputs/apk/debug/app-debug.apk
```

## Deploy and run

> The steps below assume a generic Linux or macOS host with `adb` on your PATH and the device
> connected over USB with USB debugging enabled.

### 1. Install the APK

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

If `adb install -r` fails with `INSTALL_FAILED_UPDATE_INCOMPATIBLE` (a previously installed build
was signed with a different debug key), uninstall first and reinstall:

```bash
adb uninstall com.khg.android
adb install app/build/outputs/apk/debug/app-debug.apk
```

Game data and saves under `/sdcard/Unreal` survive an uninstall.

### 2. Stage your game data

This repository ships **no game assets**. Copy your own retail *Klingon Honor Guard* data onto
the device under `/sdcard/Unreal`, preserving the original layout:

```bash
# Core data: System, Maps, Textures, Sounds, Music
adb push "Klingon Honor Guard/System"   /sdcard/Unreal/System
adb push "Klingon Honor Guard/Maps"     /sdcard/Unreal/Maps
adb push "Klingon Honor Guard/Textures" /sdcard/Unreal/Textures
adb push "Klingon Honor Guard/Sounds"   /sdcard/Unreal/Sounds

# UMX music (used for the menu and in-game tracks)
adb push "Klingon Honor Guard/Music/."  /sdcard/Unreal/Music/

# Movie files for the intro, briefings, and comm cutscenes
adb push "Klingon Honor Guard/System/"*.avi /sdcard/Unreal/System/
```

Adjust the source paths to wherever your retail installation lives. The device filesystem under
`/sdcard` is case-insensitive, so casing differences in filenames are tolerated.

### 3. Select the renderer

The renderer is chosen by an ini key. Make sure the on-device ini files request the Vulkan
device:

```ini
; in /sdcard/Unreal/System/Unreal.ini  (and Default.ini)
[Engine.Engine]
GameRenderDevice=VulkanDrv.VulkanRenderDevice
WindowedRenderDevice=VulkanDrv.VulkanRenderDevice
```

The OpenGL ES device (`NOpenGLESDrv.NOpenGLESRenderDevice`) still exists as a fallback, but the
Vulkan device is the supported path.

### 4. Launch

```bash
adb shell am start -n com.khg.android/.MainActivity
```

### Logs

The engine writes its own log to `/sdcard/Unreal/System/Unreal.log`. Android-side diagnostics
can be viewed with `adb logcat`.

## Notes

- The build uses Gradle's daemon and caching for fast incremental rebuilds. If you change a
  `CMakeLists.txt` or need a clean native build, remove the native build cache
  (`app/.cxx`) and rebuild.
- A working Gradle wrapper is assumed; `./gradlew` will download the configured Gradle
  distribution on first run.
