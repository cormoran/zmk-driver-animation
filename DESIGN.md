# DESIGN.md — zmk-driver-animation v2

Living design spec for the v2 rewrite of `zmk-driver-animation` on the
`zmk-module-template-with-custom-studio-rpc` template. This branch
(`v2-custom-studio-rpc`) will eventually replace `main`. The v1 code stays
available on branch `main` for reference (`git show main:src/...`, or a
temporary worktree: `git worktree add /tmp/v1 main`).

Status legend: `[ ]` planned, `[x]` done, `[~]` in progress.

## 1. Goals / non-goals

Goals (v2.0):

- Feature parity with v1 (all animation types, behaviors, power-source
  switching, brightness, ext-power gating, settings persistence), rebuilt on a
  clean architecture.
- Custom ZMK Studio RPC + web UI: inspect state, configure the
  boot/USB-powered/battery-powered animation selection and brightness, and
  trigger predefined animations on demand.
- Persisted state moves to `zmk-feature-custom-settings` (no bespoke settings
  blob).
- native_sim-testable with zero hardware; per-animation behavioral tests.

Non-goals (v2.0) — see docs/design/extension-ideas.md:

- New animation types (ripple/reactive, blending compositor).
- Runtime editing of per-animation parameters (colors, durations).
- Split-peripheral RPC relay (v1's split behavior for endpoint/layer-status is
  kept as-is; the RPC subsystem itself is central-only).
- Multiple independent animation engines.

## 2. Naming

| Item                | Value                                                        |
| ------------------- | ------------------------------------------------------------ |
| Zephyr module name  | `zmk-driver-animation`                                       |
| Custom subsystem id | `cormoran__animation`                                        |
| Proto package/path  | `cormoran.animation` / `proto/cormoran/animation/animation.proto` |
| Kconfig prefix      | `ZMK_ANIMATION` (`CONFIG_ZMK_ANIMATION`, `_STUDIO_RPC`, `_CUSTOM_SETTINGS`) |
| DT compats          | Keep v1 names (`zmk,animation`, `zmk,animation-solid`, `zmk,animation-compose`, `zmk,animation-battery-status`, `zmk,animation-endpoint`, `zmk,animation-layer-status`, `zmk,animation-control`, `zmk,mock-led-strip`) so existing keyboard overlays keep working |
| Behavior compats    | Keep v1 (`zmk,behavior-animation-control` = `animctl`, `zmk,behavior-animation-trigger` = `animtrig`, `zmk,behavior-animation-layer-status` = `animls`) |
| C symbol prefix     | `zmk_animation_` public API; everything else `static`        |

Drop-in compatibility with v1 devicetree overlays and Kconfig fragments is a
hard requirement; deviations must be listed in README's migration notes.

## 3. Architecture

### 3.1 Layers

```
src/
  core/
    engine.c            # tick scheduler, pixel buffer, frame pump, activity gating
    render.c             # base+overlay compositing, brightness scaling, led_strip output
    color.c              # HSL/RGB conversion + interpolation (port from v1, minus dead blend modes)
  animations/
    solid.c compose.c battery_status.c endpoint.c layer_status.c
  control/
    control.c            # state struct (single source of truth) + mutation API
    overlay_queue.c      # one-shot/ad-hoc animation queue (enqueue vs play_now)
    power_policy.c       # USB-vs-battery source detection, list selection, ext-power gating
  settings/
    settings.c           # zmk_custom_setting registration + boot apply + change listener
  studio/
    animation_handler.c  # thin RPC shell: decode -> exec -> encode (template pattern)
    request_exec.c       # transport-agnostic request execution (pmw3610 lesson)
  behaviors/
    animation_control.c animation_trigger.c animation_layer_status.c  # thin adapters
  api_stub.c              # zero-device stub so RPC/settings build on native_sim without CONFIG_ZMK_ANIMATION
include/cormoran/animation/
  animation.h            # animation vtable + frame-request API (public, for out-of-tree animations)
  control.h              # control API used by behaviors + request_exec
include/dt-bindings/zmk_driver_animation/   # keep v1 header paths for overlay compatibility
```

Dependency direction (strict, enforce in review): `behaviors`/`studio` →
`control` → `core`; `animations` → `core` only. `settings` → `control`.
Nothing in `core/` may include ZMK event headers except the activity listener
in `engine.c`; all other ZMK-core coupling (USB/BLE/battery/layers) lives in
the specific animation or in `power_policy.c`.

### 3.2 Animation model (kept from v1, cleaned)

Animations remain Zephyr devices instantiated from devicetree with a vtable:

```c
struct zmk_animation_api {
    void (*start)(const struct device *dev, uint32_t request_duration_ms);
    void (*stop)(const struct device *dev);
    void (*render_frame)(const struct device *dev, struct zmk_animation_pixel *pixels, size_t n);
    bool (*is_finished)(const struct device *dev);
};
```

Changes vs v1:

- `request_duration_ms` handling is a shared helper
  (`zmk_animation_duration_to_frames()` in `animation.h`), not a per-file
  copy-pasted ternary; every animation must honor it (v1 TODOs in solid /
  layer-status / compose).
- The cooperative `zmk_animation_request_frames()` budget model is kept (it is
  simple and power-efficient) but documented as a contract in `animation.h`,
  and `engine.c` logs a warning when the frame budget expires while
  `is_finished()` is false (v1's silent mid-animation stall).
- "Empty" animation: a proper null-object animation that initializes
  successfully and renders nothing. v1's `-ENXIO`-on-purpose init hack and all
  `device_is_ready()`-as-sentinel checks are gone; "nothing selected" is
  represented by an explicit `NULL` slot in control state.

### 3.3 Composition: one overlay mechanism

v1 had two overlapping mechanisms (compose-parallel/sequential AND the
control priority queue). v2 keeps both surfaces but with a single ownership
rule:

- `zmk,animation-compose` is an *animation* (DT-defined static composition of
  children, parallel or sequential). Unchanged semantics, honors duration.
- `control/overlay_queue.c` is the only *runtime* sequencing mechanism: a
  small queue of `{animation, duration_ms, cancelable}` records with two entry
  points, `enqueue()` (play after current overlay finishes) and `play_now()`
  (preempt cancelable overlays). Init animation, activation animation,
  low-battery alert, behavior triggers and RPC triggers all go through this
  one queue.

Render pipeline per tick (in `render.c`): reset buffer → render base
animation (selected by power policy) → render active overlay on top (an
overlay owns the pixels it writes; unwritten pixels keep the base result) →
apply global brightness for the active power source → convert to `led_rgb` →
fan out to `drivers[]` per `chain-lengths[]`.

Brightness is applied only at this output stage; animations never see it.

### 3.4 Control state — single source of truth

```c
struct zmk_animation_control_state {
    bool enabled;
    uint8_t brightness_powered;   // 0..N steps, same step semantics as v1
    uint8_t brightness_battery;
    uint8_t selected_powered;     // index into powered-animations
    uint8_t selected_battery;     // index into battery-animations
};
```

All mutations go through `control.h` API (`zmk_animation_set_enabled()`,
`zmk_animation_set_brightness()`, `zmk_animation_select()`, shift variants,
`zmk_animation_trigger(index, duration_ms, mode)`), used identically by
behaviors and RPC `request_exec.c`. No `dev0` global / `*_0` shims: control is
a singleton resolved via `DT_CHOSEN(zmk_animation)`; a `BUILD_ASSERT` rejects
multiple `zmk,animation-control` instances instead of silently ignoring them.
Every state mutation fires a `zmk_animation_state_changed` internal event that
settings (persist) and studio (notification) subscribe to.

### 3.5 Persistence via zmk-feature-custom-settings

Each field of the control state is registered as a `zmk_custom_setting` under
subsystem id `cormoran__animation` (keys: `enabled`, `brightness_powered`,
`brightness_battery`, `animation_powered`, `animation_battery`). Constraints
use `STRUCT_SECTION_ITERABLE` with plain designated initializers — NOT the
`ZMK_CUSTOM_SETTING_DEFINE` + `RANGE_INT32` macros (they don't compile on
arm-zephyr-eabi-gcc; see skills/zmk-module-dev pitfalls).

Boot-apply ordering (the classic race): `settings_load()` runs from `main()`
after all `SYS_INIT` levels and fires no changed-events. v2 therefore applies
persisted values from the control module's existing delayed boot work item
(delay controlled by the `init-animation-delay-ms` DT property, default
100 ms — same trick as pmw3610's async-init apply: correct ordering by
construction, not by priority tuning). This work item (internally
`boot_work` / `animation_control_boot_work_handler()` in control.c) is
scheduled unconditionally at device init, regardless of whether an
`init-animation` is configured — settings-apply must run on every boot, not
just boots that also enqueue an init animation. Until that work item runs,
the engine renders with DT/Kconfig defaults.

Runtime changes from the generic custom-settings web UI arrive via the
`zmk_custom_setting_changed` listener → re-apply into control state.
Changes made from behaviors/RPC go the other way: control API writes the
backing variables and requests a debounced save through the custom-settings
API. Implementation note for the coding agent: check
`dependencies/modules/zmk-feature-custom-settings` for the exact
programmatic set/save entry points before coding this; if no public save API
exists, fall back to `settings_save_one()` on the custom-settings-owned keys
and verify the generic UI picks the values up.

v1's hand-rolled settings blob (with its inverted-error-handling load bug) is
not ported.

### 3.6 Studio RPC subsystem

Registration (template pattern):

```c
ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__animation, &meta, animation_rpc_handle_request);
ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__animation, cormoran_animation_Response);
```

`security = ZMK_STUDIO_RPC_HANDLER_UNSECURED`: every operation is bounded
(indices validated against DT-fixed lists, brightness clamped), nothing gives
memory/register access. Rationale documented here so a future reviewer knows
it was a decision, not an omission; revisit if an extension adds raw pixel
writes or parameter upload.

Proto (`proto/cormoran/animation/animation.proto`, package
`cormoran.animation`; every string/bytes bounded in `.options`; no 64-bit
types; remember nanopb `has_<field> = true` on submessages):

```proto
message Request {
  oneof request_type {
    GetInfoRequest get_info = 1;
    GetStateRequest get_state = 2;
    SetEnabledRequest set_enabled = 3;
    SetBrightnessRequest set_brightness = 4;    // power_source + step value
    SelectAnimationRequest select_animation = 5; // power_source + index (SHIFT done client-side)
    TriggerAnimationRequest trigger = 6;         // index into behavior-animations, duration_ms,
                                                 // mode ENQUEUE|PLAY_NOW, cancelable
    StopOverlayRequest stop_overlay = 7;         // cancel current ad-hoc overlay
  }
}
message Response {
  oneof response_type {
    ErrorResponse error = 1;
    GetInfoResponse info = 2;    // lists of {index, name} per slot list (powered/battery/trigger),
                                 // num_pixels, fps, brightness_steps, capability flags
    StateResponse state = 3;     // full control state + current power source + active overlay index
  }
}
message Notification {
  oneof notification_type { StateResponse state_changed = 1; }
}
```

All Set*/Select/Trigger requests return the full `StateResponse` (idempotent
read-back). `Notification.state_changed` is raised from the
`zmk_animation_state_changed` event so the web UI stays live when state is
changed from the keymap. Animation display names come from a new optional
`display-name` DT property, falling back to the DT node name.

Handler split (pmw3610 lesson, adopted from day one):
`studio/animation_handler.c` only decodes/encodes and holds the static
response buffer; `studio/request_exec.c` maps decoded requests onto the
`control.h` API and is transport-agnostic (ready for a future split relay,
and directly unit-testable).

Buffer sizing: bump `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE` /
`_RX_BUF_SIZE` in the RPC test configs (defaults are 64/30 B — too small).
Size `GetInfoResponse` conservatively (max_count on lists, max_size on names
~24) and verify total encoded size < TX buffer in a unit test.

### 3.7 Behaviors

`animctl`/`animtrig`/`animls` keep their v1 DT interface and dt-bindings
header command values, but their implementations become thin calls into
`control.h`. The trigger behavior's press/hold/extend bookkeeping is kept
(fixed array + one delayed work) but moves behind the control API
(`zmk_animation_trigger_hold()/release()`), so RPC and keymap triggers share
the duration bookkeeping. `animls` (behavior-as-split-transport for the layer
bitmask) is ported as-is but quarantined inside
`animations/layer_status.c` + its behavior file, with a doc comment marking
it as a transport hack to be replaced by a relay (extension idea).

### 3.8 Split / roles

Same support level as v1: endpoint + layer-status animations are
central/peripheral aware; RPC + settings run on the central (and on
peripherals do nothing gracefully). `request_exec.c` keeps a `source`-free
signature for now; adding a `source` field to the proto later is a
backward-compatible change (proto3).

## 4. v1 defects fixed by design (do not re-introduce)

| # | v1 problem | v2 answer |
| - | ---------- | --------- |
| 1 | `animation_control.c` 957-line god file (5 concerns) | control/ split into control, overlay_queue, power_policy; settings and RPC in own dirs |
| 2 | `animation_control_play_now()` dispatches to `enqueue` vtable slot (header bug) | no user-facing control vtable at all; single `control.h` function set, unit-tested |
| 3 | Inverted settings-load error handling, untested (`CONFIG_SETTINGS=n` in build test) | custom-settings owns persistence; build test + native_sim test run with settings enabled |
| 4 | Non-static file-scope helpers with collision-prone names (`is_powered`, `set_power`, `dev0`…) | everything `static` unless declared in `include/`; enforce in review |
| 5 | Dead code: pixel-distance LUT (on by default, no consumer), blend modes + `blending-mode` DT prop, `zmk,animation-queue` binding, duplicated `CMD_BRIGHT` define, `animation_ripple_` leftovers | not ported; revival is an explicit extension idea |
| 6 | Two overlapping composition mechanisms | §3.3 single overlay queue; compose is static-only |
| 7 | 4× copy-pasted event-listener + device-fanout boilerplate | shared `ZMK_ANIMATION_DEFINE_LISTENER` helper macro in a private header |
| 8 | 4× copy-pasted duration ternary; duration TODOs unimplemented | shared helper; honoring `request_duration_ms` is part of the animation contract |
| 9 | `-ENXIO` init as "empty" sentinel; readiness-as-signal | null-object empty animation; explicit NULL slot |
| 10 | README documents `zmk,animation = &animation;` while all working configs use `&animation_control0` | `zmk,animation` chosen must point at animation-control; engine BUILD_ASSERTs/README states it; tick loop reads control, not an arbitrary node |
| 11 | Singletons hardcode DT instance 0 silently | `BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(...) <= 1)` on engine/control/layer-status |
| 12 | Behavior label `"animls"` string contract, uncheckable | resolve via DT node label at compile time where possible; otherwise a single `#define` shared by both sides + boot-time check with clear error log |

## 5. Testing strategy

- `tests/<case>/` native_sim per area: engine tick/budget, solid, compose
  (parallel + sequential), battery (mock SoC), endpoint state matrix, control
  (enable/shift/select/brightness via behaviors — port v1's case),
  overlay queue (enqueue vs play_now vs cancelable), settings persist/apply,
  studio RPC round-trips (`tests/studio/`, template pattern with
  `events.patterns` + snapshot).
- RPC/settings must build & pass with zero animation devices (api_stub.c),
  mirroring the template's `tests/test` baseline.
- `tests/zmk-config/`: port v1's `tester_xiao_animation` shield (xiao_ble +
  ws2812-spi) and build matrix: feature off / on / on+RPC
  (`studio-rpc-usb-uart` snippet) — with `CONFIG_SETTINGS=y` this time.
  `test.py` asserts .config + devicetree content per artifact (template
  style).
- `web/`: vitest for state panel + trigger flows against a mocked transport.

## 6. Implementation plan (phases for subagents)

Each phase = one commit series, green `python3 -m unittest` + pre-commit
before commit (see AGENTS.md; inside the nix devshell SKIP the broken node
hooks and run npm checks directly). Reference v1 code with
`git show main:<path>` rather than checking out files.

- **Phase A — core engine + solid + null**: core/, animations/solid.c,
  minimal control (enabled only), tests. Port color.c. Exit: native_sim
  snapshot test renders solid frames.
- **Phase B — control + behaviors + remaining animations**: overlay_queue,
  power_policy (+ ext-power gating), compose/battery/endpoint/layer_status,
  animctl/animtrig/animls behaviors, port + extend v1's behavior test. Exit:
  parity with v1 test suite and beyond (queue semantics, duration).
- **Phase C — settings**: custom-settings registration, boot apply, changed
  listener, save-on-mutate. Exit: settings tests incl. persist across
  simulated reboot (settings_load in test init).
- **Phase D — Studio RPC + web UI**: proto, handler + request_exec,
  state-changed notification, tests/studio; web UI (state panel, brightness
  sliders, per-source animation pickers, trigger buttons, live notification
  handling). Exit: `tests/studio` green, `cd web && npm test/lint/build`
  green.
- **Phase E — hardware validation on the rig** (see §7). Exit: checklist in
  §7 recorded in docs/design/hardware-validation.md with observed outputs.
- **Phase F — docs**: README rewrite (user guide incl. web UI + migration
  notes from v1), final cleanup pass.

## 7. Hardware validation plan (this rig)

Read `zmk-workspace/skills/develop-zmk-module/references/hardware-rig.md`,
`skills/debug-zmk-jlink/`, and `skills/build-zmk-config/` first. Rig facts
that shape the plan:

- Board: XIAO nRF52840 on J-Link SWD (`nRF52840_xxAA`, SWD, 4000 kHz).
  **No physical LED strip is wired** (the rig's SPI wiring is occupied by a
  PMW3610). Validation is therefore firmware-behavior-based: RTT logs +
  Studio RPC round-trips, not visual LED checks. Build the tester shield with
  the ws2812-spi node anyway (driver code paths run; the strip is simply not
  observed). Note: PMW3610 wiring uses `&xiao_d 9`/`&xiao_d 10` and SPI0 —
  if the ported tester shield maps the strip onto conflicting pins, prefer
  moving the strip's SPI to unused pins in the overlay.
- Flash with JLinkExe command file (`loadfile <build>/zephyr/zmk.hex`, `r`,
  `go`); NEVER `erase`. This unit needs `CONFIG_FLASH_LOAD_OFFSET=0x0`
  (stale non-bootloader flash below 0x27000 → stock-offset builds hardfault).
- Debug logging: RTT with the documented quirks (zero 16 bytes at
  `_SEGGER_RTT` before reset; `CONFIG_LOG_PROCESS_THREAD_STARTUP_DELAY_MS=0`;
  `CONFIG_SEGGER_RTT_BUFFER_SIZE_UP=8192`; read via JLinkExe `mem32`/`savebin`,
  not JLinkRTTLogger).
- Studio RPC from this sandbox: `PYTHONPATH=tools tools/zmk-studio-rpc` from
  the zmk-workspace root with `--transport pyusb --usb-data-interface <N>`
  (no /dev/ttyACM* here). `custom-list` must show `cormoran__animation`;
  exercise get_info/get_state/set_*/trigger via `custom-call`, verify
  StateResponse changes and RTT log lines; power-source switching can be
  simulated only partially (USB is always powered on the rig) — check the
  powered path + settings persistence across a reset (`r`+`go`, then
  get_state).

## 8. References

- v1 inventory & defect list: this file §4 + `git show main:...`
- Template usage & pitfalls: `skills/zmk-module-dev/SKILL.md`,
  `skills/zmk-module-design/SKILL.md` (in this repo)
- Worked example (handler/exec split, settings apply ordering):
  `zmk-workspace/zmk-driver-pmw3610-with-custom-studio-rpc` (DESIGN.md there)
- Extension backlog: `docs/design/extension-ideas.md`
