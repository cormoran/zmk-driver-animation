# Phase E — hardware validation results

Ran 2026-07-08 against this sandbox's shared J-Link rig (see
`zmk-workspace/docs/hardware-locking.md` and
`skills/develop-zmk-module/references/hardware-rig.md`). All hardware
commands were run under `tools/hw-lock` (owner `phase-e-anim`); locks were
released before this doc was written.

**Headline finding (read this first):** the firmware behaves correctly at
the *state* level for every RPC request exercised, and settings persistence
across reset works. But there is a real, reproducible bug in the RPC
*transport/response* path: `SetBrightness` and `SelectAnimation` calls apply
their effect (confirmed by an immediate follow-up `GetState`) but the RPC
`Response` frame for that same call is never delivered back to the host —
the client times out (tried up to 30s). The same was observed once for
`Trigger` called very soon after boot. After a timed-out call, the board's
USB CDC-ACM tty device node frequently disappeared from the host shortly
after, requiring a J-Link reset to bring it back — this may be the same
underlying issue as the pre-existing "Module Test board has a known
intermittent reset issue" note in `hardware-rig.md`, now correlated with a
specific trigger (a lost RPC response) rather than being purely random.
This is reported here per instructions, without a code fix.

## 1. Rig & board used

- Primary board: "Module Test" XIAO nRF52840, board USB serial
  `0C5B206D3B120A9F`, SWD-wired to J-Link serial `001050398082`.
- **Fallback board also used** (see §7): "Abyss Tester" XIAO nRF52840, board
  serial `10E4D16A1E4BFE9C`, J-Link serial `001057792823`. Used to
  cross-check the Trigger/StopOverlay calls and the CDC-ACM-node-disappears
  behavior after the Module Test board's tty node became persistently
  unavailable for a stretch of the session. The exact same firmware image
  (built once, flashed to both boards) was used on both; the Abyss board
  needs no flash-offset workaround but the `EXTRA_DTC_OVERLAY_FILE` (offset-0
  partition) build still flashes and boots on it without issue.
- Both boards + both J-Links were locked for the whole session
  (`tools/hw-lock acquire --owner phase-e-anim --task "phase E animation hw
  validation" jlink-001050398082 jlink-001057792823 zmk-10E4D16A1E4BFE9C`,
  plus `zmk-0C5B206D3B120A9F` and later `zmk-10E4D16A1E4BFE9C` acquired as
  each board's USB identity appeared/was needed).

## 2. Build

Build command that succeeded (from the repo root, inside the nix devshell):

```
cd /home/ubuntu/zmk-workspace/zmk-driver-animation
OVL="$(pwd)/tests/zmk-config/config/hw_validation.overlay"
west zmk-build tests/zmk-config \
  -b 'xiao_ble//zmk' \
  -s tester_xiao_animation \
  -a hw_validation \
  -S studio-rpc-usb-uart \
  -p always \
  --cmake-args " -DEXTRA_DTC_OVERLAY_FILE=${OVL} \
    -DCONFIG_ZMK_STUDIO=y \
    -DCONFIG_ZMK_ANIMATION=y \
    -DCONFIG_ZMK_ANIMATION_STUDIO_RPC=y \
    -DCONFIG_SETTINGS=y \
    -DCONFIG_ZMK_CUSTOM_SETTINGS=y \
    -DCONFIG_ZMK_ANIMATION_CUSTOM_SETTINGS=y \
    -DCONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y \
    -DCONFIG_USE_SEGGER_RTT=y \
    -DCONFIG_LOG=y \
    -DCONFIG_LOG_BACKEND_RTT=y \
    -DCONFIG_LOG_BACKEND_UART=n \
    -DCONFIG_SEGGER_RTT_BUFFER_SIZE_UP=8192 \
    -DCONFIG_LOG_PROCESS_THREAD_STARTUP_DELAY_MS=0 "
```

Result: `All builds succeeded.` Build directory:
`build/hw_validation__xiao_ble/zmk__tester_xiao_animation`. Memory usage:
FLASH 311156 B (32.19%), RAM 90544 B (34.54%). The west log's own UF2
conversion line already confirmed the load address:

```
Converted to uf2, output size: 622592, start address: 0x0
```

Double-checked independently with objdump against the ELF (toolchain at
`/nix/store/.../zephyr-sdk-0.16.9/arm-zephyr-eabi/bin/`):

```
$ arm-zephyr-eabi-objdump -f build/hw_validation__xiao_ble/zmk__tester_xiao_animation/zephyr/zmk.elf | grep -i 'start address'
start address 0x0000f42d
```

Low address, confirming the `hw_validation.overlay`'s `code_partition@0x0`
redefinition took effect (not the stock 0x27000 `code_partition` offset).
`_SEGGER_RTT` symbol address (from `arm-zephyr-eabi-nm`): `0x20002010`.

## 3. Flash procedure

SWD-flashed with a real command file (JLinkExe cannot read `/dev/stdin`),
never using `erase`:

```
SelectEmuBySN 001050398082
device nRF52840_xxAA
si SWD
speed 4000
connect
w4 0x20002010, 0x00000000
w4 0x20002014, 0x00000000
w4 0x20002018, 0x00000000
w4 0x2000201C, 0x00000000
loadfile .../zephyr/zmk.hex
r
go
exit
```

(The four `w4` writes zero the `_SEGGER_RTT` control block's 16-byte
signature before reset so RTT re-initializes cleanly — a documented rig
quirk, `AIRCR.SYSRESETREQ` does not clear RAM.)

Output confirmed: `J-Link: Flash download: Bank 0 @ 0x00000000: 1 range
affected (311296 bytes)` ... `O.K.`, followed by a clean `r`/`go` with no
errors. Same procedure (different `SelectEmuBySN` serial) used to flash the
Abyss fallback board later in the session — also succeeded (`311296 bytes`,
`O.K.`).

## 4. Boot RTT evidence

Read via `JLinkExe`'s `mem32`/`savebin` (JLinkRTTLogger/JLinkRTTClient don't
find the control block on this rig, as documented). Real captured lines from
the first boot after flashing:

```
[00:00:00.295,928] <inf> zmk: ZMK animation control ready (3 powered, 3 battery, 1 behavior animations)
*** Booting Zephyr OS build 9df4b12b5af3 ***
[00:00:00.302,642] <inf> fs_nvs: 8 Sectors of 4096 bytes
[00:00:00.302,642] <inf> fs_nvs: alloc wra: 0, f88
[00:00:00.302,642] <inf> fs_nvs: data wra: 0, a4
[00:00:00.304,473] <inf> bt_hci_core: HW Platform: Nordic Semiconductor (0x0002)
[00:00:00.396,453] <inf> zmk: overlay queue: enqueued animation_solid_1
[00:00:00.429,595] <inf> zmk: Start animation solid
[00:00:00.474,639] <inf> usb_hid: Device resumed
[00:00:00.474,731] <inf> usb_hid: Device reset detected
[00:00:00.657,623] <inf> bt_hci_core: Identity: EA:7D:49:6E:DE:B4 (random)
[00:00:00.993,774] <inf> usb_hid: Device configured
[00:00:00.993,988] <dbg> zmk: get_selected_transport: USB is preferred and ready
[00:00:00.994,049] <inf> zmk: Endpoint changed: USB
[00:00:01.420,227] <wrn> zmk: Animation frame budget expired while animation reports unfinished
[00:00:01.420,288] <inf> zmk: Stop animation solid
[00:00:31.305,236] <inf> zmk: Stop animation solid
```

Confirms: animation control subsystem initialized (3 powered / 3 battery / 1
behavior animation, matching the shield's DT config), the boot overlay
animation (`animation_solid_1`) ran, and USB HID configured normally. The
second "Stop animation solid" at `t=31.3s` is ZMK's standard ~30s activity
idle timeout stopping the base animation — see §5's Trigger/StopOverlay
notes, this matters for those calls.

Later, on the reset-without-reflash used for the persistence test (§6), a
fresh boot log line confirms the settings boot-apply path:

```
[boot] <inf> zmk: animation settings: apply enabled=1 brightness=6/1 animation=1/0
[boot] <inf> zmk: animation control: select index 0->1
```

(exactly the persisted values set over RPC before that reset — see §6).

## 5. Studio RPC round-trip

`custom-list` confirmed the subsystem (proto package is `cormoran.animation`,
per `grep '^package' proto/cormoran/animation/animation.proto`):

```
[0] cormoran__animation ui=https://cormoran.github.io/zmk-driver-animation/
[1] cormoran_custom_settings ui=https://cormoran.github.io/zmk-feature-custom-settings/
```

All 6 request types below were sent via:

```
PYTHONPATH=tools tools/zmk-studio-rpc --workspace /home/ubuntu/zmk-workspace \
  --transport serial --port /dev/zmk-hp-zmk-tty-<serial>-00 \
  custom-call --identifier cormoran__animation \
  --proto zmk-driver-animation/proto/cormoran/animation/animation.proto \
  --request-type cormoran.animation.Request \
  --response-type cormoran.animation.Response \
  --json '<request>'
```

### GetInfo — clean success

Request: `{"getInfo":{}}`

```json
{
  "info": {
    "powered_animations": [
      {"name": "animation_solid_0"},
      {"index": 1, "name": "animation_solid_1"},
      {"index": 2, "name": "animation_layer_status"}
    ],
    "battery_animations": [
      {"name": "animation_solid_0"},
      {"index": 1, "name": "animation_solid_1"},
      {"index": 2, "name": "animation_layer_status"}
    ],
    "behavior_animations": [{"name": "animation_solid_1"}],
    "num_pixels": 3,
    "fps": 30,
    "brightness_steps": 10
  }
}
```

### GetState — clean success (baseline)

Request: `{"getState":{}}`

```json
{
  "state": {
    "enabled": true,
    "brightness_powered": 1,
    "brightness_battery": 1,
    "is_powered": true
  }
}
```

(`selected_powered`/`selected_battery`/`overlay_active` omitted = proto3
default `0`/`false`, as expected at first boot.)

### SetBrightness — **applies but the response never arrives**

Request: `{"setBrightness":{"powerSource":"POWER_SOURCE_POWERED","step":3}}`

```
Timed out waiting for a Studio RPC frame
```

(exit code 4, waited up to 30s in one retry — never returned). A follow-up
`GetState` immediately after, however, shows the value *did* change:

```json
{"state": {"enabled": true, "brightness_powered": 3, "brightness_battery": 1, "is_powered": true}}
```

Repeated 3 more times with different steps (5, 7, 2, 6) across both boards —
every single `SetBrightness` call timed out on the response, and every one
of them still applied (confirmed each time via a follow-up `GetState`).
This is 100% reproducible in this session, not a one-off flake.

### SelectAnimation — same pattern: applies, response never arrives

Request: `{"selectAnimation":{"powerSource":"POWER_SOURCE_POWERED","index":1}}`

```
Timed out waiting for a Studio RPC frame
```

Follow-up `GetState`:

```json
{"state": {"enabled": true, "brightness_powered": 3, "brightness_battery": 1, "selected_powered": 1, "is_powered": true}}
```

`selected_powered` went from unset(0) to `1` as requested. Reproduced again
later with index 2 and index 1 — same timeout-but-applied pattern every
time.

### Trigger — one clean success, one timeout (see idle-timeout note below)

Request: `{"trigger":{"index":0,"durationMs":1000,"mode":"TRIGGER_MODE_PLAY_NOW","cancelable":true}}`

First attempt (Module Test board, run a couple of minutes after boot, so
past ZMK's ~30s activity idle timeout — animation module's internal
`running` flag was false by then):

```json
{
  "state": {
    "enabled": true,
    "brightness_powered": 5,
    "brightness_battery": 1,
    "selected_powered": 1,
    "is_powered": true
  }
}
```

Response arrived in well under 1s — a clean success — but `overlay_active`
is absent (= false): reading `src/control/control.c`,
`zmk_animation_trigger()` early-returns `0` (no-op, no `notify_state_changed`)
when `!zmk_animation_control_is_running()`, and `is_running()` is
`data->enabled && data->running`, where `data->running` is cleared by ZMK's
own ~30s activity-idle timeout (matches the boot log's second `Stop
animation solid` at `t=31.3s`). This is by-design power-saving behavior, not
a bug — but it does mean Studio RPC `Trigger`/`StopOverlay` calls are
silent no-ops whenever the board has been idle >~30s, which could surprise
a Studio UI user (worth a design note for a future phase: whether an RPC
`Trigger` should itself count as "activity").

Second attempt, run immediately (~1.5s) after a fresh reset on the Abyss
fallback board, specifically to catch the animation in its "running" state
before the 30s timeout:

```
Timed out waiting for a Studio RPC frame
```

— i.e. the same response-loss bug as `SetBrightness`/`SelectAnimation`, this
time on `Trigger`. Shortly after this timeout, the board's CDC-ACM tty node
disappeared from `/dev/zmk-hp-zmk-tty-*` and did not come back without a
J-Link reset (see headline finding).

### StopOverlay — clean success

Request: `{"stopOverlay":{}}`

```json
{
  "state": {
    "enabled": true,
    "brightness_powered": 6,
    "brightness_battery": 1,
    "selected_powered": 1,
    "is_powered": true
  }
}
```

Response arrived normally. No `overlay_active` field (already false/idle —
consistent with the same 30s-idle gating noted above, so this was a
no-op-but-successful-round-trip case, not a state change to verify).

### RTT cross-check for the RPC calls

The 8KB RTT buffer wraps very quickly (ZMK's `zmk_usb_get_conn_state` debug
log fires extremely often — every ~50-300ms while USB is connected), so by
the time a dump was taken a minute or more after a given RPC call, that
call's own log lines had already been overwritten. What *was* visible
repeatedly:

```
<dbg> zmk: zmk_rpc_custom_subsystem_encode_response_payload: Encoding custom response of size 174   (GetInfo)
<dbg> zmk: zmk_rpc_custom_subsystem_encode_response_payload: Encoding custom response of size 10     (GetState)
```

confirming the firmware *did* run its encode path for the calls that
returned successfully. No equivalent "Encoding custom response" line was
ever captured for a `SetBrightness`/`SelectAnimation`/timed-out-`Trigger`
call before the buffer wrapped past it — consistent with (but not, on its
own, proof of) the response encode/send for those specific calls never
completing.

## 6. Settings persistence across reset

1. Set distinctive values over RPC: `SetBrightness` (POWERED, step=6) and
   `SelectAnimation` (POWERED, index=1). Both showed the "applies, response
   times out" pattern from §5. Confirmed applied via `GetState`:
   ```json
   {"state": {"enabled": true, "brightness_powered": 6, "brightness_battery": 1, "selected_powered": 1, "is_powered": true}}
   ```
2. Explicitly persisted via the custom-settings module's own RPC subsystem
   (`cormoran_custom_settings`, `SaveSettingsRequest` with an empty scope =
   save everything) — this module's settings write-through is
   MEMORY-only-on-mutate by design (see `src/settings/animation_settings.c`'s
   "save-on-mutate design" comment), so an explicit save is required for
   persistence, matching the documented design:
   ```json
   {"status": {"affected_count": 5, "message": "Settings saved"}}
   ```
   (This ran cleanly the first time, saving the *previous* values 2/2. The
   second `SaveSettings` call, made after setting the new 6/1 values, itself
   hit the same response-timeout bug — see below.)
3. Reset the board via JLinkExe (`r` + `go`, RTT signature re-zeroed first),
   **without reflashing**.
4. Boot RTT log after reset:
   ```
   <inf> zmk: animation settings: apply enabled=1 brightness=6/1 animation=1/0
   <inf> zmk: animation control: select index 0->1
   ```
5. RPC `GetState`/`StopOverlay` after reset (once the board's tty node was
   available again) confirmed the same values over the wire:
   ```json
   {"state": {"enabled": true, "brightness_powered": 6, "brightness_battery": 1, "selected_powered": 1, "is_powered": true}}
   ```

**Result: PASS.** `brightness_powered=6` and `selected_powered=1` survived
the reset, both via RTT boot-apply log and via a live RPC read-back
afterward — this confirms the Phase C boot-apply path and the
custom-settings persistence wiring both work correctly on real hardware.
Notably, this also means the second `SaveSettingsRequest` call (the one
targeting the 6/1 values) **did** actually flash-write successfully despite
its RPC response never reaching the host — the same "applies, response
lost" pattern as §5's `SetBrightness`/`SelectAnimation`.

## 7. Deviations, issues, follow-ups

- **Primary finding (see top of doc): lost RPC responses for
  `SetBrightness`/`SelectAnimation`/(once) `Trigger`/(once) `SaveSettings`.**
  Reproducible across both boards and across the whole session — every
  `SetBrightness` and `SelectAnimation` call in this session (7+ calls
  total) applied its effect but never returned a Response frame to the
  host, even with a 30s client timeout. `GetInfo`/`GetState`/(most)
  `Trigger`/`StopOverlay`/(first) `SaveSettings` all round-tripped
  normally. Not root-caused to a specific line during this session (no
  fix attempted, per instructions) but circumstantial evidence points at
  something in the synchronous `notify_state_changed()` →
  `animation_state_changed_callback()` → `raise_zmk_studio_custom_notification()`
  path that these two request handlers (and, at least once, `Trigger`)
  exercise while still inside the RPC `call()` handler's own response-encode
  context, contending for or otherwise disrupting the pending Response send
  for that same call — worth a focused firmware-side investigation in a
  follow-up phase, ideally with an in-tree logic analyzer/USB capture rather
  than RTT (whose 8KB buffer wraps too fast to catch the relevant window).
- **Correlated USB CDC-ACM tty-node dropouts.** On both boards, the
  `/dev/zmk-hp-zmk-tty-*` node disappeared from the host more than once
  during the session, always following (within a few seconds) a call that
  had just timed out, and required a J-Link `r`+`go` reset to reappear —
  never recovered by waiting alone (tried up to ~30s multiple times). This
  matches `hardware-rig.md`'s pre-existing note about the Module Test
  board's "known intermittent reset issue", but this session's evidence
  suggests it may not be random — it may be the host-side USB stack giving
  up on an endpoint that the firmware has stopped servicing because of the
  same response-path issue above. Flagging for whoever investigates that
  bug: check whether the USB CDC ACM IN endpoint itself gets wedged.
- **Idle-timeout gating on Trigger/StopOverlay is by design, not a bug**,
  but is worth a product note: `zmk_animation_trigger()` and
  `zmk_animation_trigger_stop()` both silently no-op (return `0`, no
  notification) once ZMK's ~30s activity-idle timeout has stopped the base
  animation (`data->running == false`). A Studio UI operator triggering an
  overlay on an idle device would see no visible effect and (given the RPC
  response-loss bug above) may not even get a Response back to tell them
  why. Consider whether a future phase should have RPC-driven mutations
  count as "activity" for this module, independent of the response-loss fix.
- No physical LED strip is wired on this rig (as noted in DESIGN.md §7), so
  as planned, this validation is entirely RTT-log- and RPC-response-based,
  not a visual check.
- Both boards' CDC-ACM enumeration timing was unpredictable enough that
  several `Trigger`/`StopOverlay`/persistence-check attempts had to be
  retried with fresh resets; this consumed a large share of the session's
  hardware time.
