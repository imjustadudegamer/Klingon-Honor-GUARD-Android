# Controller Configuration

The port is built for gamepad play, using a native direct-input path for the analog sticks. The
button layout follows a console-FPS convention (left stick to move, right stick to look, triggers
to fire). Most of the configuration is **ini-tunable with no rebuild**.

## Where the settings live

- **Button binds** live in `/sdcard/Unreal/System/Unreal.ini` under `[Engine.Input]`
  (`Joy1`…`Joy16`, `JoyPov*`). These are normal engine input binds, so you can remap them by
  editing the ini.
- **Stick sensitivity, deadzones, and the look filter** live under `[NSDLDrv.NSDLClient]`.
  Note that `Default.ini` can override `Unreal.ini` for some of these keys, so edit the value in
  the file your install actually reads; the on-device ini value overrides the compiled-in
  default (the code default only seeds a fresh install).

## Stick sensitivity & look filter (`[NSDLDrv.NSDLClient]`)

| Key | Purpose |
|---|---|
| `AndroidNativeDirectInput` | Enables the native direct-stick input path. |
| `AndroidNativeLeftStickScale` | Left-stick (movement) sensitivity multiplier. Note: movement is speed-capped, so high values saturate quickly and feel twitchy. |
| `AndroidNativeRightStickScale` | Right-stick (look) sensitivity multiplier. |
| `AndroidNativeRightStickSmoothing` | Enables the right-stick jitter low-pass (look filter). `False` = raw look; `True` = filtered. |
| `ScaleXYZ` | Left/move axis scale. |
| `ScaleRUV` | Right/look axis scale. |
| *(deadzones)* | Per-stick deadzone thresholds. |

Sensitivity values are clamped to a sane range in code. Tune them live by editing the ini and
relaunching.

### Low-latency right-stick look filter

The right-stick jitter low-pass is **adaptive** rather than a flat smoothing factor. A flat
low-pass added noticeable aim lag (several frames to settle). Instead, the filter's blend factor
ramps from light smoothing near the deadzone edge up to near-instantaneous as deflection
increases, and fast flicks snap through immediately. This suppresses edge jitter while keeping
aim latency low.

## Button layout

Binds map the SDL gamepad buttons to engine `Joy*` actions. The default console-FPS-style
mapping is:

| Control | Action |
|---|---|
| Left stick | Move |
| Right stick | Look |
| Right trigger (RT) | Fire |
| Left trigger (LT) | Alt-fire |
| A | Jump |
| B | Duck |
| X | Grab / interact |
| Y | Next weapon |
| Back | Pause |
| Left bumper (L1) | Previous weapon |
| Right bumper (R1) | Next weapon |
| D-pad left / right | Previous / next weapon |
| D-pad up | Inventory next |
| D-pad down | Inventory activate |

The triggers (Fire / Alt-fire) are routed through the native direct-input path; the rest are
ordinary ini binds, so you can change the layout by editing `[Engine.Input]` in `Unreal.ini`.

## 2D UI scale

The on-screen UI and font size scale together via a `UIScale` value (read from
`AndroidUI.ini` `UIScale=`). This is a single layout-safe lever for making text and HUD larger on
small handheld screens.
