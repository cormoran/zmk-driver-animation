# zmk-driver-animation

![ZMK Version](https://img.shields.io/badge/ZMK-main-blue)
[![Test](https://github.com/cormoran/zmk-driver-animation/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-driver-animation/actions/workflows/zmk-module.yml)

A ZMK module that drives an RGB LED strip: solid/battery/BLE-endpoint/layer
status/composed base animations selected per power source (USB vs battery),
ad-hoc "overlay" animations you can trigger from the keymap or from a web UI,
brightness control, optional settings persistence, and a custom ZMK Studio
web app to control everything live over USB/BLE.

This is the v2 rewrite of the module (see [`DESIGN.md`](./DESIGN.md) for the
full architecture). If you used the old (pre-rewrite) version, read
[Migration from v1](#migration-from-v1) below — devicetree/keymap
compatibility was a hard design goal, but a few things did change.

## Features

- **Base animations per power source**: pick one animation to run while
  USB-bus-powered and a separate one while battery-powered (e.g. a bright
  static color on USB, a dimmer breathing color on battery).
- **Built-in animation types** (`src/animations/`): solid color with cycling
  (`zmk,animation-solid`), battery-percentage bar graph + low-battery alert
  (`zmk,animation-battery-status`), BLE/USB endpoint status
  (`zmk,animation-endpoint`), active-layer indicator
  (`zmk,animation-layer-status`), a static parallel/sequential composition of
  other animations (`zmk,animation-compose`), and a null "nothing selected"
  animation (`zmk,animation-none`).
- **Ad-hoc overlay animations**: any `behavior-animations` entry can be
  played on top of the current base animation via a keymap behavior or a
  Studio RPC call, either queued to play after the current overlay
  (`ENQUEUE`) or immediately pre-empting a cancelable one (`PLAY_NOW`), for a
  given duration.
- **Brightness control** per power source, in discrete steps.
- **Keymap behaviors**: `animctl` (enable/disable, brightness shift, select
  animation), `animtrig` (press/hold/release-aware overlay trigger), `animls`
  (split-peripheral layer-status relay — an internal transport, not a
  general-purpose behavior).
- **Optional persistence** via
  [`zmk-feature-custom-settings`](https://github.com/cormoran/zmk-feature-custom-settings):
  enabled state, both brightness levels, and both selected base animations
  survive a reboot once saved.
- **Optional custom ZMK Studio RPC + web UI**: inspect live state, drag
  brightness sliders, pick base animations per source, and fire overlay
  triggers from a browser, with live push updates when state changes from
  the keymap.
- **Split keyboards**: endpoint/layer-status animations are central- and
  peripheral-aware (peripheral shows only central-connection status; the
  layer bitmask is relayed from central to peripheral via `animls`). The
  Studio RPC/settings subsystems themselves are central-only; see
  [`DESIGN.md`](./DESIGN.md) §3.8.

## Requirements

- A ZMK build. Studio RPC support requires **a patched ZMK fork with custom
  Studio RPC support** (`cormoran/zmk`, branch `main+custom-studio-protocol`)
  — vanilla ZMK does not have the custom-subsystem RPC hooks this module
  uses. You only need the patched fork if you enable
  `CONFIG_ZMK_ANIMATION_STUDIO_RPC`; the animation engine itself, behaviors,
  and DT-defined animations work on stock ZMK.
- Persistence additionally requires
  [`zmk-feature-custom-settings`](https://github.com/cormoran/zmk-feature-custom-settings)
  (pulled in automatically, see Installation below) and `CONFIG_SETTINGS=y`.
- The web UI requires Node.js and consumes
  [`@cormoran/zmk-studio-react-hook`](https://github.com/cormoran/react-zmk-studio).
- An LED strip driver implementing Zephyr's LED Strip API (e.g.
  `worldsemi,ws2812-spi`).

## Installation

Add this module (and, if you want Studio RPC, the patched ZMK fork) to your
`config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-driver-animation
      remote: cormoran
      revision: main # or a pinned commit hash
      import: true # also pulls in zmk-feature-custom-settings automatically
    # Only needed if you enable CONFIG_ZMK_ANIMATION_STUDIO_RPC:
    - name: zmk
      remote: cormoran
      revision: main+custom-studio-protocol
      import:
        file: app/west.yml
```

Then enable the feature in your `config/<shield>.conf`:

```conf
CONFIG_ZMK_ANIMATION=y

# Optional: custom Studio RPC + web UI
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_ANIMATION_STUDIO_RPC=y
# GetInfoResponse can exceed the RPC framework's small default buffers;
# bump these if the web UI reports a decode/encode failure.
CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128
CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=128

# Optional: persist enabled/brightness/animation-selection across reboots
CONFIG_SETTINGS=y
CONFIG_ZMK_CUSTOM_SETTINGS=y
CONFIG_ZMK_ANIMATION_CUSTOM_SETTINGS=y
# Optional: expose the generic custom-settings save/import/export RPC too
CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y
```

### Kconfig reference (user-relevant options)

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_ZMK_ANIMATION` | n | Master switch; selects `LED_STRIP`. |
| `CONFIG_ZMK_ANIMATION_FPS` | 30 | Engine tick rate, 1-60. |
| `CONFIG_ZMK_ANIMATION_STOP_ON_IDLE` | y | Stop rendering (and gate `ext-power`) while ZMK is idle/asleep. |
| `CONFIG_ZMK_ANIMATION_STUDIO_RPC` | n | Registers the `cormoran__animation` custom Studio RPC subsystem. Requires `CONFIG_ZMK_STUDIO` and the patched ZMK fork. |
| `CONFIG_ZMK_ANIMATION_CUSTOM_SETTINGS` | n | Registers control state as `zmk-feature-custom-settings` entries (subsystem `cormoran__animation`). Requires `CONFIG_ZMK_CUSTOM_SETTINGS`. |
| `CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM` | 10 | Max number of simultaneously-held `animtrig` overlay slots. (Renamed from v1's misspelled `..._MAX_PARALELISM` — see migration notes.) |
| `CONFIG_ZMK_ANIMATION_TRIGGER_MAX_DURATION_MS` | 10000 | Clamp on a triggered overlay's play duration. |
| `CONFIG_ZMK_ANIMATION_TRIGGER_MIN_DURATION_MS` | 1000 | Minimum play duration for a triggered overlay. |
| `CONFIG_ZMK_ANIMATION_TRIGGER_EXTEND_MS_ON_HOLD` | 500 | How much a held `animtrig` extends the remaining duration by, per tick. |
| `CONFIG_ZMK_ANIMATION_MOCK_LED_STRIP` | n | Mock LED strip driver for native_sim testing only — not for real keyboards. |

## Devicetree configuration

A minimal working example (adapted from
[`tests/zmk-config/boards/shields/tester_xiao_animation`](./tests/zmk-config/boards/shields/tester_xiao_animation/tester_xiao_animation.overlay),
a shield that actually builds in this repo's test matrix):

```dts
#include <dt-bindings/led/led.h>
#include <zmk_driver_animation/animation.dtsi>
#include <behaviors/animation_control.dtsi>
#include <behaviors/animation_trigger.dtsi>
#include <dt-bindings/zmk_driver_animation/animation_control.h>

&my_spi_bus {
    status = "okay";
    led_strip: ws2812@0 {
        compatible = "worldsemi,ws2812-spi";
        reg = <0>;
        spi-max-frequency = <4000000>;
        chain-length = <3>;
        spi-one-frame = <0x70>;
        spi-zero-frame = <0x40>;
        color-mapping = <LED_COLOR_ID_GREEN LED_COLOR_ID_RED LED_COLOR_ID_BLUE>;
    };
};

/ {
    chosen {
        /* Both MUST point at the same animation-control node: the engine
         * renders whichever device `zmk,animation` resolves to, and
         * `zmk,animation-control` is used internally (e.g. by the
         * low-battery alert) to enqueue overlays. */
        zmk,animation = &animation_control0;
        zmk,animation-control = &animation_control0;
    };

    animation: animation {
        compatible = "zmk,animation";
        drivers = <&led_strip>;
        chain-lengths = <3>;
        pixels = <&pixel 0 0>, <&pixel 1 0>, <&pixel 2 0>;
    };

    animation_solid_green: animation_solid_green {
        compatible = "zmk,animation-solid";
        pixels = <0 1 2>;
        colors = <HSL(120, 100, 50)>;
    };

    animation_solid_rainbow: animation_solid_rainbow {
        compatible = "zmk,animation-solid";
        pixels = <0 1 2>;
        duration = <5>;
        colors = <HSL(0, 100, 50) HSL(240, 100, 50)>;
    };

    animation_control0: animation_control_0 {
        compatible = "zmk,animation-control";
        /* Selectable base animations per power source, and the list
         * `animtrig`/Studio Trigger requests index into: */
        powered-animations = <&animation_solid_green &animation_solid_rainbow>;
        battery-animations = <&animation_solid_green &animation_solid_rainbow>;
        behavior-animations = <&animation_solid_rainbow>;
        /* Optional: play briefly right after boot. */
        init-animation = <&animation_solid_rainbow>;
        init-animation-duration-ms = <1000>;
    };
};
```

Key nodes/properties:

- `zmk,animation` (the engine): `drivers` (phandles to LED-strip devices),
  `chain-lengths` (LED count per driver, same order), `pixels` (per-pixel
  `<&pixel x y>` phandle-array — `&pixel` comes from
  `zmk_driver_animation/animation.dtsi`).
- `zmk,animation-control` (the singleton control node — exactly one is
  allowed, enforced with a `BUILD_ASSERT`): `powered-animations` /
  `battery-animations` (base-animation lists, selected by index),
  `behavior-animations` (overlay list, indexed by `animtrig`/Studio
  `Trigger`), `init-animation(-duration-ms)`, `activation-animation(-duration-ms)`
  (played on resume from idle), `ext-power` (phandle to a power switch,
  disabled while idle/nothing selected), `brightness-steps`,
  `max-brightness`, `default-powered-brightness`,
  `default-battery-brightness`, `queue-size` (overlay queue capacity,
  default 16).
- Every animation node accepts an optional `display-name` string, shown by
  the Studio RPC `GetInfo` response / web UI instead of the raw DT node name.
- `zmk,animation-layer-status` also needs `default-color` and/or `colors`
  (per-layer HSL) plus `layer-offset` if you split layer indicators across
  a split keyboard's two halves; see
  [`dts/zmk_driver_animation/animation_layer_status.dtsi`](./dts/zmk_driver_animation/animation_layer_status.dtsi)
  for the `animls` behavior wiring it needs on the peripheral.

## Keymap behaviors

```dts
#include <dt-bindings/zmk_driver_animation/animation_control.h>
#include <dt-bindings/zmk_driver_animation/animation_trigger.h>

/ {
    keymap {
        compatible = "zmk,keymap";
        default_layer {
            bindings = <
                &animctl ANIMATION_CONTROL_CMD_ENABLE 1     // enable
                &animctl ANIMATION_CONTROL_CMD_SHIFT 1      // next base animation
                &animctl ANIMATION_CONTROL_CMD_SELECT 0     // select base animation by index
                &animctl ANIMATION_CONTROL_CMD_BRIGHT 1      // brighten current power source by 1 step
                &animtrig ANIMATION_TRIGGER_CMD_TRIGGER 0    // play behavior-animations[0] as an overlay
            >;
        };
    };
};
```

Shorthand macros are also available (`ANM_EN`/`ANM_DS`, `ANM_INC`/`ANM_DEC`,
`ANM_SEL`, `ANM_BRI`/`ANM_BRD`, `ANM_TRG`) — see
[`include/dt-bindings/zmk_driver_animation/animation_control.h`](./include/dt-bindings/zmk_driver_animation/animation_control.h)
and
[`animation_trigger.h`](./include/dt-bindings/zmk_driver_animation/animation_trigger.h).
`animctl`'s `SELECT`/`SHIFT`/`BRIGHT` commands always act on the *currently
active* power source; `animtrig`'s second param is the index into
`behavior-animations`, and holding the key extends the overlay's remaining
duration (`CONFIG_ZMK_ANIMATION_TRIGGER_EXTEND_MS_ON_HOLD`) instead of
immediately ending it on release.

## Persistence (`zmk-feature-custom-settings`)

With `CONFIG_ZMK_ANIMATION_CUSTOM_SETTINGS=y`, every keymap/RPC-driven change
(enable, both brightnesses, both selected base animations) is written
through to the custom-settings registry in **MEMORY** mode immediately —
so a connected generic custom-settings UI reflects the change right away —
but it is **not** written to flash automatically on every change (deliberate:
brightness-shift/select are hot, repeatable keymap actions, and writing
flash on every step would be slow and wear it out). Flash persistence
requires an explicit **save**, via the generic
`zmk-feature-custom-settings` Studio RPC/web UI's save action (or its
`settings_save_one()`-equivalent), which then survives a reboot and is
re-applied by this module's own boot-time apply step.

**Known caveat:** with `CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y`, the generic
custom-settings `SaveSettings` RPC call currently times out waiting for its
response over USB — a hardware-confirmed bug in that dependency's own RPC
path (see [`docs/design/hardware-validation.md`](./docs/design/hardware-validation.md),
"RPC-path suppression re-validation" section). **The setting is still
written to flash and does persist across reboot; only the RPC
acknowledgement to the client is lost**, so a web UI "Save" action may
appear to hang or fail even though it worked — check with a `GetState`/
reboot rather than trusting the save button's own success indicator. This
module's *own* RPC calls (`SetBrightness`, `SelectAnimation`, `Trigger`,
`StopOverlay`, etc.) are unaffected and return promptly; only the separate
custom-settings `SaveSettings` call has this issue.

## Web UI

The `web/` app is a small React + TypeScript client for the
`cormoran__animation` custom Studio RPC subsystem, using
[`@cormoran/zmk-studio-react-hook`](https://github.com/cormoran/react-zmk-studio):

```bash
cd web
npm ci
npm run generate   # generates TypeScript types from proto/cormoran/animation/animation.proto
npm run dev        # local dev server
# or: npm run build
```

Connect to your keyboard over serial (USB) from the app's "Connect Serial"
button; it needs `CONFIG_ZMK_ANIMATION_STUDIO_RPC=y` (and `CONFIG_ZMK_STUDIO=y`)
on the firmware. Once connected, the animation panel lets you:

- See live state: enabled/disabled, current power source (USB/battery), and
  the active overlay (with a "Stop overlay" button).
- Drag brightness sliders for the powered and battery sources independently.
- Pick the base animation for each power source from buttons built from the
  firmware's `GetInfo` animation lists.
- Fire any `behavior-animations` entry as a 3-second, play-now, cancelable
  overlay with one click.
- See read-only capability info: pixel count, FPS, brightness steps.

State updates pushed by the firmware (e.g. from a keymap behavior, or the
power source changing) are reflected live via a Studio notification
subscription — no polling.

## Migration from v1

The devicetree compatibles (`zmk,animation`, `zmk,animation-solid`,
`zmk,animation-compose`, `zmk,animation-battery-status`,
`zmk,animation-endpoint`, `zmk,animation-layer-status`,
`zmk,animation-control`) and behavior compatibles/command values
(`animctl`/`animtrig`/`animls` and all `ANIMATION_CONTROL_CMD_*`/
`ANIMATION_TRIGGER_CMD_*` values) are unchanged — most existing v1 overlays
and keymaps keep working as-is. What did change:

- **`zmk,animation` chosen node must point at the `animation-control` node,
  not a bare animation node.** v1's own README showed
  `zmk,animation = &animation;` (a plain animation node) while every real v1
  config actually pointed it at `&animation_control0` — v2 makes this the
  only correct wiring and enforces it structurally (the tick loop reads
  control state, not an arbitrary chosen device). If your v1 overlay
  followed the old README literally, repoint `zmk,animation` at your
  `zmk,animation-control` node.
- **`label` is no longer required** on the `zmk,animation-control` node (v1's
  binding required it); it's fine to drop it.
- **`zmk,animation-empty` was renamed `zmk,animation-none`** (same "renders
  nothing" null-object semantics, now implemented as a proper empty
  animation instead of an `-ENXIO`-on-purpose init hack).
- **`zmk,animation-queue` and the `blending-mode` DT property are gone.**
  Both were dead code in v1 (no consumer); if you referenced either, they
  have no effect and can be removed. (Tracked as extension ideas if you need
  them revived: see [`docs/design/extension-ideas.md`](./docs/design/extension-ideas.md).)
- **`CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALELISM` (typo) was renamed to
  `CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM`.** Update your `.conf` if
  you set this Kconfig symbol directly (most users only bind the behavior
  and never touch this symbol).
- **`CONFIG_ZMK_ANIMATION_PIXEL_DISTANCE` (the pairwise pixel-distance
  lookup table, on by default in v1) is gone** — it had no consumer in v1
  either; a future reactive/ripple animation type would reintroduce it (see
  extension ideas).
- **New optional `display-name` property** on every animation node, shown by
  the (new) Studio RPC/web UI in place of the DT node name.
- **Entirely new in v2, opt-in, no v1 equivalent:** the custom Studio RPC
  subsystem (`CONFIG_ZMK_ANIMATION_STUDIO_RPC`) + web UI, and settings
  persistence via `zmk-feature-custom-settings`
  (`CONFIG_ZMK_ANIMATION_CUSTOM_SETTINGS`) — v1 had no RPC/web surface and no
  persistence at all (state always reset to DT/Kconfig defaults on reboot).
- Internally, the public C headers moved from `zmk_driver_animation/*.h` to
  `cormoran/animation/*.h` and the single 957-line `animation_control.c` was
  split into `control/`, `settings/`, `studio/`, `behaviors/`. This only
  matters if you wrote an out-of-tree animation type against v1's internal
  API; DT-bindings header paths
  (`dt-bindings/zmk_driver_animation/...`) used by overlays/keymaps are
  unchanged.

## Further reading

- [`DESIGN.md`](./DESIGN.md) — full v2 architecture: layers, animation
  model, the single overlay-queue composition rule, control state, Studio
  RPC subsystem details, and the list of v1 defects this rewrite fixes.
- [`docs/design/extension-ideas.md`](./docs/design/extension-ideas.md) —
  deferred features (runtime animation parameters, reactive/ripple
  animations, a real blending compositor, split-peripheral RPC relay, and
  more).
- [`docs/design/hardware-validation.md`](./docs/design/hardware-validation.md)
  — real hardware validation logs (RTT + Studio RPC round-trips) from this
  module's development rig, including the `SaveSettings` timeout
  investigation referenced above.
- [`web/README.md`](./web/README.md) — web app development details
  (project layout, `ts-proto` codegen, test helpers).

## Development

See [`DESIGN.md`](./DESIGN.md) §6-7 for the implementation/test plan this
module was built against. To build/test this module itself (not just
consume it as a dependency):

```bash
git clone <this repository>
cd zmk-driver-animation
west init -l west --mf west-test-isolated.yml
west update --narrow
west zephyr-export

# Run unit test + build test and verify the results
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
# Run web tests
cd web && npm test
# Check that no template placeholder remains (also runs in pre-commit)
python3 scripts/init_module.py --verify-only
```

Every commit needs to pass `pre-commit run` (formatting + tests).
