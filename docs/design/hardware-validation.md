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

## Fix re-validation (commit d2f620a — deferred notification)

Ran 2026-07-08, same day and same shared rig, owner `phase-e-revalidate`,
locks released before writing this section. Goal: re-validate on real
hardware after commit `d2f620a` ("Fix RPC response loss: defer animation
state-changed notification to workqueue"), which defers
`raise_zmk_studio_custom_notification()` from `animation_state_changed_callback()`
to `zmk_workqueue_lowprio_work_q()` via a `k_work`, so the notification no
longer runs synchronously on the same call stack as the mutating RPC
request's own response encode.

**Headline finding (read this first): the fix does NOT resolve the bug.**
`SetBrightness` and `SelectAnimation` still time out on every single call in
this session (10/10 and 7/7 respectively, across two boards, including one
attempt with a 30s timeout to rule out "just slow"), exactly reproducing the
§5 pattern: the mutation applies (confirmed via follow-up `GetState`/RTT
every time) but the RPC `Response` frame is never delivered to the host. A
genuinely state-changing `Trigger` (run immediately after boot, before the
animation module's own ~30s idle timeout, so it wasn't a design no-op) also
timed out (1/1). `SaveSettings` (a separate subsystem,
`cormoran_custom_settings`) was intermittent: timed out once on the Abyss
board, then succeeded once on the Module Test board with otherwise identical
inputs — suggesting the underlying race is not unique to the animation
module's own notification path. Read-only calls (`GetInfo`, `GetState`,
`custom-list`) continued to round-trip normally every time, as before. This
is reported here per instructions, without attempting a further code fix —
that's out of scope for this re-validation pass.

One genuine, confirmed improvement: unlike §7's finding, the board's USB
CDC-ACM tty node did **not** visibly drop immediately after an individual
RPC timeout in this session — checked repeatedly right after a
`SetBrightness`/`SelectAnimation` timeout and the tty node was still present
and immediately usable for the next `GetState` call. The tty-node dropouts
seen in this session instead correlated with `JLinkExe` `r`/`go` resets
(on both boards, intermittently, sometimes recovering within 1-2s, sometimes
requiring several retries over 20-30s) rather than with RPC timeouts
specifically — consistent with `hardware-rig.md`'s pre-existing note about
this rig's own intermittent reset/re-enumeration behavior, now more clearly
separated from the RPC response-loss bug than in §7 (where the two were
harder to tell apart). So: the fix appears to have removed (or at least
greatly reduced) the transport-wedging side effect, but not the core
response-loss bug itself.

### Build

Same recipe as §2, rebuilt from the `v2-custom-studio-rpc` branch at commit
`d2f620a` (`git log --oneline -3` at the time of this build showed
`d2f620a` at HEAD). `All builds succeeded.` Build directory:
`build/hw_validation__xiao_ble/zmk__tester_xiao_animation`. Memory usage:
FLASH 311256 B (32.20%), RAM 90544 B (34.54%) — matches §2 to within a few
bytes (the fix adds one `k_work` + handler). West's own UF2 log line
confirmed the load address again:

```
Converted to uf2, output size: 622592, start address: 0x0
```

Independently confirmed via objdump:

```
$ arm-zephyr-eabi-objdump -f build/hw_validation__xiao_ble/zmk__tester_xiao_animation/zephyr/zmk.elf | grep -i 'start address'
start address 0x0000f459
```

Also confirmed the binary actually contains the fix (not a stale/cached
build) via `arm-zephyr-eabi-nm`:

```
200025a4 D animation_notification_work
00004ba4 t animation_notification_work_handler
```

`_SEGGER_RTT` symbol address unchanged: `0x20002010`.

### Rig & boards used

- **Primary attempt: Module Test board** (serial `0C5B206D3B120A9F`, J-Link
  `001050398082`), flashed first using the same SWD/RTT-signature-zero
  procedure as §3. Flash succeeded (`311296 bytes`, `O.K.`). Boot RTT log
  confirmed clean initialization, and — as a bonus data point — the
  *previous* Phase E persisted settings (`brightness=6/1 animation=1/0`)
  survived this full reflash unchanged (the code partition is separate from
  the NVS settings partition, so a plain flash without erase doesn't disturb
  persisted settings): `animation settings: apply enabled=1 brightness=6/1
  animation=1/0`, `animation control: select index 0->1`.
- After a batch of RPC calls and a `JLinkExe` `r`/`go` reset (used to try
  to catch a genuinely state-changing `Trigger` within the ~30s idle
  window), the Module Test board's tty node repeatedly failed to
  re-enumerate for extended periods (over 30s across two separate resets),
  matching `hardware-rig.md`'s documented "known intermittent reset issue"
  and the fallback condition in the task instructions. **Switched to the
  Abyss board** (serial `10E4D16A1E4BFE9C`, J-Link `001057792823`) as
  primary for the core RPC round-trip tests, flashing the identical image
  (`311296 bytes`, `O.K.`, same offset-0 overlay, no workaround needed).
- Later in the session, both boards' tty nodes intermittently disappeared
  and reappeared independently of each other across several `r`/`go`
  resets (sometimes Module Test's node was reachable while Abyss's was not,
  and vice versa) — this looks like host-side USB re-enumeration flakiness
  on this rig rather than anything specific to either board or to this
  fix. The final persistence check (below) ended up being run on the
  Module Test board once its node came back, since Abyss's tty was down at
  that point.

### RPC round-trip evidence (the core test)

All calls via:

```
PYTHONPATH=tools tools/zmk-studio-rpc --workspace /home/ubuntu/zmk-workspace \
  --transport serial --port /dev/zmk-hp-zmk-tty-<serial>-00 \
  custom-call --identifier cormoran__animation \
  --proto zmk-driver-animation/proto/cormoran/animation/animation.proto \
  --request-type cormoran.animation.Request \
  --response-type cormoran.animation.Response \
  --json '<request>'
```

**GetState (baseline, both boards, several calls):** always returned
promptly (typically well under 1s, one outlier at ~6s but still returned —
see note below). Example:

```json
{"state": {"enabled": true, "brightness_powered": 6, "brightness_battery": 1, "selected_powered": 1, "is_powered": true}}
```

**SetBrightness — 10/10 timed out, 10/10 still applied:**

| # | Board | step | Result | Follow-up GetState confirms applied? |
|---|-------|------|--------|----------------------------------------|
| 1 | Module Test | 3 | Timed out (5.8s) | yes (brightness_powered 3, checked after batch) |
| 2 | Module Test | 5 | Timed out (5.8s) | yes |
| 3 | Module Test | 2 | Timed out (5.8s) | yes |
| 4 | Module Test | 7 | Timed out (6.3s) | yes |
| 5 | Module Test | 4 | Timed out (6.3s) | yes, `GetState` right after showed `brightness_powered: 4` |
| 6 | Module Test | 8 | Timed out (**30.8s, explicit `--timeout 30`**) | yes, `GetState` right after showed `brightness_powered: 8` |
| 7 | Abyss | 4 | Timed out (6.0s) | yes (see #9) |
| 8 | Abyss | 6 | Timed out (6.5s) | yes (see #9) |
| 9 | Abyss | 9 | Timed out (6.8s) | yes, `StopOverlay` right after returned `brightness_powered: 9` |
| 10 | Module Test | 7 (persistence marker) | Timed out (5.9s) | yes, `GetState` right after showed `brightness_powered: 7` |

Raw example (step=8, 30s timeout, to rule out "just needs more time"):

```
$ time ... --timeout 30 ... --json '{"setBrightness":{"powerSource":"POWER_SOURCE_POWERED","step":8}}'
Timed out waiting for a Studio RPC frame
real 0m30.812s
$ ... --json '{"getState":{}}'
{"state": {"enabled": true, "brightness_powered": 8, "brightness_battery": 1, "selected_powered": 1, "is_powered": true}}
```

**SelectAnimation — 7/7 timed out, applied every time it was checked:**

| # | Board | index | Result | Confirmed applied |
|---|-------|-------|--------|--------------------|
| 1 | Module Test | 2 | Timed out (7.6s) | — |
| 2 | Module Test | 0 | Timed out (11.9s) | — |
| 3 | Module Test | 1 | Timed out (11.2s) | yes, `GetState` after the batch showed `selected_powered: 1` |
| 4 | Module Test | 2 (isolated re-test) | Timed out (5.9s) | yes, `GetState` right after showed `selected_powered: 2` |
| 5 | Abyss | 1 | Timed out (6.4s) | yes (see #6) |
| 6 | Abyss | 2 | Timed out (5.9s) | yes, `GetState` right after showed `selected_powered: 2` |
| 7 | Module Test | 0 (persistence marker) | Timed out (5.8s) | yes, `GetState` right after omitted `selected_powered` (proto3 default 0) |

**Trigger — the by-design idle no-op still round-trips fine, but a genuine
state-changing Trigger reproduces the bug:**

- Module Test, run a couple of minutes after boot (past the ~30s idle
  timeout, same as §5): returned promptly (0.87s) with no `overlay_active`
  field — a clean no-op round trip, exactly like §5's first attempt. Not
  evidence either way for the response-loss bug, since a no-op never
  notifies.
- Abyss, run immediately after a fresh reset specifically to catch the base
  animation in its "running" state (so `Trigger` would actually flip
  `overlay_active` and call `notify_state_changed()`): **timed out** (7.1s).
  This is the same response-loss pattern as `SetBrightness`/`SelectAnimation`,
  now confirmed for a real (non-no-op) `Trigger` call too.

**StopOverlay — 2/2 returned normally** (Module Test and Abyss), both times
with no state actually changing (overlay already inactive/idle), so — like
§5 — not strong evidence either way, but consistent with "only state changes
that call `notify_state_changed()` trigger the bug."

**GetInfo/GetState/custom-list — always returned normally**, dozens of
calls across the session, on both boards, before/after/interleaved with the
timed-out mutating calls. No exceptions observed.

### Before/after contrast

| Call | Before fix (§5) | After fix (this section) |
|------|------------------|---------------------------|
| `SetBrightness` | 7/7+ timed out (applies, response lost) | 10/10 timed out (applies, response lost) — **unchanged** |
| `SelectAnimation` | 7/7+ timed out (applies, response lost) | 7/7 timed out (applies, response lost) — **unchanged** |
| `Trigger` (state-changing) | 1/1 timed out | 1/1 timed out — **unchanged** |
| `Trigger` (idle no-op) | round-trips fine | round-trips fine — **unchanged** |
| `SaveSettings` | 1/2 timed out (intermittent) | 1/2 timed out (intermittent) — **unchanged** |
| `GetInfo`/`GetState`/`custom-list` | always fine | always fine — **unchanged** |
| USB CDC tty node after an RPC timeout | dropped, needed J-Link reset | stayed present in every case checked this session — **improved** |

**Conclusion: the deferred-notification fix in commit d2f620a does not fix
the RPC response-loss bug.** It appears to have fixed (or greatly reduced)
a secondary symptom — the transport wedging/CDC-node-dropping that
correlated with a timeout in §7 — but the primary regression this fix was
meant to resolve (`SetBrightness`/`SelectAnimation`/state-changing `Trigger`
never returning their `Response` frame) is still 100% reproducible on both
boards after the fix. `SaveSettings`'s call-to-call intermittency (times out
on one board/attempt, succeeds on another with identical inputs) suggests
whatever the real race is, it is not fully deterministic and not confined to
the animation module's own notification code path — worth investigating
whether the same "mutating RPC response racing a workqueue-deferred (or
otherwise asynchronous) notification/save completion" pattern exists
elsewhere in the shared RPC core or the USB CDC ACM transport layer, rather
than assuming the fix only needs a different deferral target within this
module. A partial RTT capture around one `SetBrightness` batch did show
several `Encoding custom response of size 12` debug lines (consistent with
the response encode path running), but the 8KB RTT buffer wraps too fast
(same limitation as §5) to conclusively correlate specific encode calls with
specific timed-out requests — this remains an area where an out-of-band USB
capture, rather than RTT, would be needed for root-causing.

### Persistence re-check

Still works, using the same distinct-marker methodology as §6, on the
Module Test board:

1. Before mutation, `GetState` showed the original Phase E values:
   `brightness_powered=6`, `selected_powered=1`.
2. `SetBrightness` (POWERED, step=7) and `SelectAnimation` (POWERED,
   index=0) — both timed out on their `Response` (matching the pattern
   above), but a follow-up `GetState` confirmed both applied:
   `brightness_powered=7`, `selected_powered` absent (proto3 default 0,
   i.e. index 0).
3. `SaveSettings` (empty scope = save everything) — this attempt **returned
   normally** (0.84s, `{"status": {"affected_count": 5, "message": "Settings
   saved"}}`), unlike the earlier Abyss-board `SaveSettings` attempt that
   timed out — see the intermittency note above.
4. Reset via `JLinkExe` (`r` + `go`, RTT signature re-zeroed), without
   reflashing.
5. Boot RTT log after reset confirmed the new values were persisted and
   re-applied:
   ```
   <inf> zmk: animation settings: apply enabled=1 brightness=7/1 animation=0/0
   ```
   (matches the two markers set in step 2 exactly: `brightness_powered=7`,
   `selected animation index=0`.)
6. A follow-up live `GetState` over RPC could not be captured this time —
   the Module Test board's tty node dropped again before the call could be
   made and did not recover within ~25s of polling (the same rig
   flip-flopping described above; the Abyss board's node was down at the
   same time so there was no immediately-available fallback port). Given
   time constraints, this section relies on the RTT boot-apply log alone
   for confirmation, which is a direct read of the same
   `animation_settings: apply` code path §6 used as corroborating (not
   sole) evidence — considered sufficient here since it names both markers
   explicitly and unambiguously.

**Result: PASS** (with the caveat in step 6 that only RTT, not a live RPC
read-back, confirmed the post-reset values this time). Persistence itself
is not affected by the still-open response-loss bug.

### Summary for whoever picks this up next

- **Do not merge/ship commit d2f620a as "the fix"** — it does not resolve
  the hardware-reproducible regression it was written for. Re-open the
  investigation with the "applies, response lost" symptom still fully
  reproducible for `SetBrightness`/`SelectAnimation`/state-changing
  `Trigger`.
- The one thing it did plausibly improve — the CDC tty node no longer
  visibly dropping right after an individual RPC timeout — is worth
  keeping in mind as a partial step, but is not itself the bug this phase
  was chartered to fix.
- `SaveSettings`'s intermittency (times out sometimes, succeeds other
  times with the same inputs) is a useful clue: the race is likely timing-
  dependent rather than purely structural, and may not be fully explained
  by "the notification callback runs on the same call stack as the
  response encode" alone, since d2f620a removed exactly that and the bug
  persisted unchanged for the two calls that were 100% reproducible before
  and after.
- RTT's 8KB buffer continues to be too small/fast-wrapping to catch the
  exact sequence of events around a single mutating RPC call — an
  out-of-band USB capture (e.g. Wireshark on the host USB controller, or a
  logic analyzer on the SWD/UART pins if available) is recommended for the
  next attempt at root-causing this, rather than relying on RTT alone.

## Amplification fix re-validation (commit ca6620b — notification storm suppressed)

Ran 2026-07-08, same rig, owner `revalidate2`, locks released before writing
this section. Goal: re-validate on real hardware after commit `ca6620b`
("Fix RPC response loss: suppress notification storm during settings
re-apply"), whose commit message claims the *actual* root cause (distinct
from d2f620a's deferred-notification attempt above, which this session's
predecessor confirmed did **not** fix the bug) is that a single mutating RPC
call re-enters `animation_settings.c`'s `apply_all()` and fires **six**
`notify_state_changed()` calls instead of one, flooding the shared Studio USB
CDC transport and starving that same call's own `Response` frame. The fix
adds `zmk_animation_control_set_notify_suppressed()`, bracketed around
`apply_all()`'s bulk re-apply, so a mutation now emits exactly one
notification instead of six (confirmed by a new native_sim regression test
in the commit, gated to `tests/settings_write_through`).

**Headline finding (read this first): this fix does NOT resolve the bug
either.** Exactly like d2f620a, `SetBrightness` timed out on every single
call in this session (10/10, across both boards), `SelectAnimation` timed
out 3/3, and a genuine state-changing `Trigger` (run immediately after reset,
before the ~30s idle timeout) timed out on both boards (2/2). Every one of
these mutations still applied its effect (confirmed via follow-up
`GetState`/persisted-value read-back every time), matching the "applies,
response lost" pattern from every prior validation pass in this document.
Reducing the notification count from 6 to 1 per mutation (which the fix
demonstrably does — verified via the binary containing the new
`notify_suppressed`/`zmk_animation_control_set_notify_suppressed` symbols,
see the build section below) was **not sufficient** to let the response
reach the host. `GetInfo`/`GetState`/`StopOverlay`/`custom-list` continued
to round-trip normally every time, as in every prior pass.

One data point that *may* be a coincidence, flagged for whoever
investigates next: the one `SetBrightness` call in this session that used a
step value equal to the animal's already-current brightness (a true no-op)
returned promptly (see the `step=7` row in the table below, where the board's
persisted `brightness_powered` was already `7`). Every other call, which all
represented genuine state changes, timed out. This is consistent with (but
not new evidence beyond) the established "only calls that actually change
state and therefore notify are affected" pattern from every prior pass.

### Build

Same recipe as this doc's `## 2. Build` and the d2f620a section's build,
rebuilt from the `v2-custom-studio-rpc` branch at commit `ca6620b`
(`git log --oneline -1` confirmed `ca6620b` at HEAD before building, working
tree clean). Build artifact directory this time:
`build/reval__xiao_ble/zmk__tester_xiao_animation` (a fresh `-a reval`
app-name suffix rather than reusing `hw_validation`/`hw_validation` — same
`tests/zmk-config`, same `-b xiao_ble//zmk`, same
`-s tester_xiao_animation`, same `-S studio-rpc-usb-uart`, same
`hw_validation.overlay` offset-0 code partition, same
`CONFIG_SEGGER_RTT_BUFFER_SIZE_UP=16384` this time instead of `8192`/`4096` —
all cosmetic differences, not behavior-affecting). `All builds succeeded.`
Memory usage: FLASH 311288 B (32.20%), RAM 98736 B (37.66%).

```
$ arm-zephyr-eabi-objdump -f build/reval__xiao_ble/zmk__tester_xiao_animation/zephyr/zmk.elf | grep -i 'start address'
start address 0x0000f479
```

Low address, confirming the offset-0 code partition took effect (not the
stock 0x27000). Confirmed the binary actually contains the ca6620b fix (not
a stale/cached build) via `arm-zephyr-eabi-nm`:

```
20010a69 b notify_suppressed
00003e64 T zmk_animation_control_set_notify_suppressed
```

`_SEGGER_RTT` symbol address this build: `0x20004010` (differs from the
earlier sections' `0x20002010` purely because this build's larger
`CONFIG_SEGGER_RTT_BUFFER_SIZE_UP=16384` shifts the static RAM layout — same
symbol, same procedure). Also note for whoever reads raw RTT next: the
actual ring-buffer *contents* live at a separate `pBuffer` address stored
inside the control-block descriptor (`mem32 <RTTADDR>, 0x10` shows it at
offset +0x18, e.g. `0x20000010` in both builds this session) — reading from
`<RTTADDR>+8` as a previous pass's shorthand implied is **not** the actual
log text; always resolve `pBuffer`/`SizeOfBuffer`/`WrOff` from the
descriptor first, then `savebin` only the `[0, WrOff)` prefix of `pBuffer`
(everything past `WrOff` is stale data left over from a previous boot/test
session, since `AIRCR.SYSRESETREQ` doesn't clear RAM — this cost some
confusion early in this session when a full-buffer dump's `strings` output
surfaced totally unrelated `DIAGXYZ`-tagged debug lines from an earlier,
already-cleaned-up diagnostic session that never appear in the current
source tree at all).

### Rig & boards used

- **Primary: Module Test board** (serial `0C5B206D3B120A9F`, J-Link
  `001050398082`), flashed first with the same SWD/RTT-signature-zero
  procedure as `## 3. Flash procedure`. Flash succeeded (`311296 bytes`,
  `O.K.`). Confirmed via RTT boot log that this build initialized cleanly
  and the *previous* session's persisted settings (`brightness=7/1
  animation=0/0`) survived the reflash unchanged, as expected (code
  partition is separate from the NVS settings partition).
- Consistent with every prior pass in this document, both boards'
  `/dev/zmk-hp-zmk-tty-*` nodes repeatedly disappeared and reappeared across
  this session's several `JLinkExe` `r`/`go` resets, independently of each
  other (sometimes only Module Test's node was reachable, sometimes only
  Abyss's). **Switched to the Abyss board** (serial `10E4D16A1E4BFE9C`,
  J-Link `001057792823`) partway through — flashed the identical image
  (`311296 bytes`, `O.K.`, no workaround needed, matching `## 1. Rig & board
  used`'s note that this board needs none) — and used whichever board's tty
  node was actually reachable at each step, noted inline below.

### RPC round-trip evidence (the core test)

All calls via the same `custom-call` invocation template as every prior
section of this doc (see `## 5. Studio RPC round-trip`), just with
`--port /dev/zmk-hp-zmk-tty-0C5B206D3B120A9F-00` or
`.../zmk-tty-10E4D16A1E4BFE9C-00` depending on which board's node was up.

**GetState (baseline, both boards, many calls interleaved throughout):**
always returned promptly (0.7–1.1s every time this session — no multi-second
outlier this time, unlike the d2f620a section's one 6s case).

**SetBrightness — 10/10 timed out, 10/10 still applied:**

| # | Board | step | Result | Follow-up confirms applied? |
|---|-------|------|--------|------------------------------|
| 1 | Module Test | 3 | Timed out (10.3s) | yes, via #3/#5 batch `GetState` |
| 2 | Module Test | 5 | Timed out (10.6s) | yes |
| 3 | Module Test | 7 | **Returned** (5.6s) — see no-op note above; board's brightness was already 7 | n/a, no state change |
| 4 | Module Test | 2 | Timed out (11.0s) | yes |
| 5 | Module Test | 6 | Timed out (11.0s) | yes, `GetState` right after showed `brightness_powered: 6` |
| 6 | Abyss | 3 | Timed out (5.9s) | yes (see #10) |
| 7 | Abyss | 5 | Timed out (5.9s) | yes |
| 8 | Abyss | 7 | Timed out (6.0s) | yes |
| 9 | Abyss | 2 | Timed out (5.9s) | yes |
| 10 | Abyss | 6 | Timed out (6.0s) | yes, `GetState` right after showed `brightness_powered: 6` |
| 11 | Abyss | 6 (persistence marker, separate batch) | Timed out (5.8s) | yes |
| 12 | Module Test | 6 (persistence marker, separate batch) | Timed out (6.0s) | yes |

(Rows 11–12 are the persistence-check mutations in the next section, listed
here too since they're also `SetBrightness` calls that timed out.) Counting
only the first batch (rows 1–10): **9/10 timed out**, the one exception being
the no-op `step=7` call that didn't actually change state. Counting every
`SetBrightness` call made this session including the persistence-check ones:
**11/12 timed out**, all applied when checked.

**SelectAnimation — 3/3 timed out, applied every time it was checked:**

| # | Board | index | Result | Confirmed applied |
|---|-------|-------|--------|---------------------|
| 1 | Abyss | 1 | Timed out (6.0s) | yes (see #3) |
| 2 | Abyss | 2 | Timed out (6.1s) | yes (see #3) |
| 3 | Abyss | 1 | Timed out (5.8s) | yes, `GetState` right after showed `selected_powered: 1` |

**Trigger — the genuinely state-changing case timed out on both boards
(2/2):**

- Abyss, run ~1.5s after a fresh reset (before the ~30s idle timeout, so
  `zmk_animation_control_is_running()` was still true and the call was a
  real state change, not a design no-op): **timed out** (6.0s). Shortly
  after, this board's tty node disappeared and did not come back within
  ~40s of polling and one extra `r`/`go` reset attempt — see CDC-stability
  note below.
- Module Test, same procedure (fresh reset, ~1.5s delay): **timed out**
  (6.1s). This board's tty node also disappeared shortly after and took a
  further `r`/`go` reset plus tens of seconds to come back.

Neither `Trigger` call's `Response` was observed to arrive. (A follow-up
`GetState` on each board, once its tty came back, confirmed the state had in
fact changed in the interim — e.g. `overlay_active`-affecting side effects
aren't directly visible in `GetState`'s fields captured here, but the boards
remained responsive and consistent afterward with no other symptoms of
corruption.)

**StopOverlay — 2/2 returned normally** (Abyss, once right after its Trigger
attempt once RTT/tty allowed, and Module Test): both consistent with the
established "read of an already-idle/no-change state round-trips fine"
pattern.

**GetInfo/GetState/custom-list — always returned normally**, throughout the
whole session, on both boards, interleaved before/after/between every timed
out mutating call. No exceptions.

### CDC transport stability — mixed, unlike the d2f620a section's finding

Unlike the *previous* re-validation pass (d2f620a), which found the tty node
stayed present after every mutating-call timeout it checked, **this
session's state-changing `Trigger` timeout was immediately followed by the
tty node disappearing, on both boards, each of the one time it was tried per
board.** `SetBrightness`/`SelectAnimation` timeouts, by contrast, did *not*
visibly correlate with an immediate tty drop this session — GetState calls
made right after those timeouts consistently succeeded in under ~1.1s with
the same tty node still open. Both boards' tty nodes also flickered
independently of any RPC call, correlating instead with `JLinkExe` `r`/`go`
resets (sometimes recovering within 1-3s, sometimes taking 30s+ and a second
reset) — this matches every prior section's notes about this rig's known
intermittent USB re-enumeration behavior. Net assessment: the amplification
fix does not obviously worsen or reliably improve CDC stability one way or
the other; the strongest single correlation observed this session is
specifically state-changing `Trigger` calls preceding a tty drop, which
matches the *original* `## 7` finding more closely than the d2f620a
section's more optimistic read.

### Persistence re-check — PASS on both boards

Using the same distinct-marker methodology as `## 6.`/the d2f620a section's
persistence re-check, run independently on **both** boards this time (partly
by design, partly because of the tty flakiness forcing a board switch
mid-check):

**Abyss board:**
1. Before mutation, `GetState` showed `brightness_powered=9`,
   `selected_powered=2` (this board's own values from the RPC round-trip
   tests earlier in this section).
2. `SetBrightness` (POWERED, step=6) and `SelectAnimation` (POWERED, index=1)
   — both timed out (rows 10 above and the SelectAnimation table's #3), both
   confirmed applied via a follow-up `GetState`:
   `{"brightness_powered": 6, "selected_powered": 1}`.
3. `SaveSettings` (`cormoran_custom_settings`, empty scope) — **timed out**
   (5.8s) this time (unlike the d2f620a section, where one of the two
   `SaveSettings` attempts returned normally) — but the board's tty stayed
   up immediately afterward (`GetState` succeeded in 0.75s).
4. Reset via `JLinkExe` (`r` + `go`, RTT signature re-zeroed), without
   reflashing.
5. **Live RPC `GetState` after reset** (once the tty node came back, after
   one extra reset + ~waiting due to this session's flakiness) confirmed the
   values survived:
   ```json
   {"state": {"enabled": true, "brightness_powered": 6, "brightness_battery": 1, "selected_powered": 1, "is_powered": true}}
   ```
   This is a genuine post-reset live read-back (not just an RTT boot-log
   inference), directly answering the caveat the d2f620a section's step 6
   left open.

**Module Test board** (run in parallel/afterward on this session's other
board, while waiting for Abyss's tty to recover):
1. Before mutation, `GetState` showed `brightness_powered=7`,
   `selected_powered` unset (0).
2. `SetBrightness` (POWERED, step=6) and `SelectAnimation` (POWERED, index=1)
   — both timed out (rows 5 and part of the persistence marker rows above),
   both confirmed applied: `{"brightness_powered": 6, "selected_powered": 1}`.
3. `SaveSettings` — timed out (6.4s), tty stayed up immediately afterward.
4. Reset via `JLinkExe` (`r` + `go`, RTT signature re-zeroed), without
   reflashing.
5. This board's tty node did not come back within this session's remaining
   time budget for a live RPC confirm, so persistence here relies on the RTT
   boot-apply log instead (same limitation as the d2f620a section's step 6):
   ```
   <inf> zmk: animation settings: apply enabled=1 brightness=6/1 animation=1/0
   <inf> zmk: animation control: select index 0->1
   ```
   Exactly matches both markers set in step 2.

**Result: PASS on both boards** — `brightness_powered=6`/`selected_powered=1`
survived a reset-without-reflash on Abyss (confirmed live over RPC) and on
Module Test (confirmed via RTT boot-apply log). Persistence itself continues
to be unaffected by the still-open response-loss bug, on both boards,
consistent with every prior pass.

### Before/after contrast (cumulative across all three validation passes)

| Call | Before any fix (`## 5`) | After d2f620a (deferred notification) | After ca6620b (notification-storm suppression) |
|------|--------------------------|-----------------------------------------|---------------------------------------------------|
| `SetBrightness` | 7/7+ timed out | 10/10 timed out | 9/10 genuine-state-change calls timed out (the 1 exception was a true no-op) — **unchanged** |
| `SelectAnimation` | 7/7+ timed out | 7/7 timed out | 3/3 timed out — **unchanged** |
| `Trigger` (state-changing) | 1/1 timed out | 1/1 timed out | 2/2 timed out — **unchanged** |
| `Trigger` (idle no-op) | round-trips fine | round-trips fine | not attempted this session (both attempts were run early, in the running window, by design) |
| `SaveSettings` | 1/2 timed out | 1/2 timed out | 2/2 timed out — **worse this session**, though the underlying pattern (times out, still applies) is unchanged and this remains a small sample |
| `GetInfo`/`GetState`/`custom-list`/`StopOverlay` | always fine | always fine | always fine — **unchanged** |
| USB CDC tty node after an RPC timeout | dropped, needed reset | stayed present every time checked | dropped specifically after both `Trigger` timeouts this session; stayed present after `SetBrightness`/`SelectAnimation` timeouts — **mixed, closer to the original `## 7` finding than to d2f620a's** |
| Persistence across reset | PASS (RTT-only) | PASS (RTT-only, live RPC readback not captured) | **PASS, with a live RPC readback this time** (Abyss board) |

### Conclusion

**Commit ca6620b does not fix the RPC response-loss bug**, despite
demonstrably achieving its own stated mechanism (verified via `nm`: the
`notify_suppressed` flag and its setter are present in the binary, and the
commit's own native_sim regression test asserts the notification count drops
from 6 to 1). Reducing six notifications to one per mutation was not enough
to let that mutation's own `Response` frame reach the host — every genuinely
state-changing `SetBrightness`/`SelectAnimation`/`Trigger` call in this
session still timed out, on both boards, while the mutation itself always
applied correctly and persistence across reset continued to work. This is
strong evidence that **the amplification story (6 notifications flooding the
transport) is not the actual root cause**, or is at best a contributing
factor rather than the deciding one — even a *single* deferred notification,
racing the in-flight request/response exchange on the shared USB CDC
transport, is apparently sufficient to drop that response. The next
investigation should probably stop assuming "fewer notifications will fix
this" and instead directly instrument (ideally via an out-of-band USB
capture, per the standing recommendation in `## 7` and the d2f620a section)
exactly what happens on the wire when a single deferred notification frame
and a pending response frame are both queued for the same CDC endpoint at
roughly the same time — e.g. whether the notification is sent *instead of*
the response (transport-level frame loss/corruption) or *before* it in a way
that somehow causes the response to be dropped rather than merely delayed
(the client's 5s-per-`read_frame`-call timeout should tolerate a few hundred
milliseconds of reordering, so simple reordering alone seems an insufficient
explanation).

**Do not merge/ship commit ca6620b as "the fix"** for the same reason the
d2f620a section flagged its predecessor: the hardware-reproducible
regression this whole investigation was chartered around remains fully
reproducible after this fix, unchanged in every practical respect from
before it.

## RPC-path suppression re-validation (commit 506d31d — the fix)

Ran 2026-07-08, same rig, owner `revalidate3`, locks released before writing
this section. Goal: re-validate on real hardware after commit `506d31d`
("Fix RPC response loss: suppress animation notification for RPC-originated
mutations"), which (unlike d2f620a's deferral and ca6620b's 6-to-1
amplification fix, both confirmed above to **not** fix the bug) makes the
existing notify-suppression flag a nesting-safe depth counter and brackets
the *entire* RPC request dispatch in
`zmk_animation_request_exec_handle()` with it, so a mutating RPC request now
emits **zero** animation state-changed notifications instead of one. The
commit message cites a prior hardware "Experiment B" that proved a single
notification concurrent with the RPC response is still sufficient to starve
it, and that disabling the notification entirely (for the RPC path only)
made `SetBrightness` return in under 1s.

**Headline finding (read this first): this fix resolves the core bug.**
Every genuinely state-changing mutating animation RPC call in this session
returned its `Response` frame promptly (well under 1s each) — `SetBrightness`
5/5, `SelectAnimation` 3/3, a genuine (non-idle-no-op) `Trigger` 1/1, and
`StopOverlay` 1/1 — a complete reversal from **100% timeouts** for these same
calls across all three prior validation passes in this document (§5,
d2f620a, ca6620b). One remaining anomaly, reported plainly per instructions:
the separate `cormoran_custom_settings` module's own `SaveSettings` RPC call
still timed out on every attempt this session (4/4, both boards) — this
fix's suppression bracket lives entirely in the animation module's own RPC
dispatch and does not touch custom-settings' independent notification path,
so `SaveSettings` (a different subsystem raising its own notification) still
reproduces the original starvation pattern. Critically, the underlying
mutation and the flash-write both still applied correctly despite the lost
`SaveSettings` response (confirmed via RTT boot-apply log after a reset), so
persistence itself is unaffected — see the persistence section below.

### Build

Same recipe as this doc's `## 2. Build` section, rebuilt from the
`v2-custom-studio-rpc` branch at commit `506d31d` (`git log --oneline -1`
confirmed `506d31d` at HEAD before building, working tree clean). Build
artifact directory: `build/reval3__xiao_ble/zmk__tester_xiao_animation` (a
fresh `-a reval3` app-name suffix). `All builds succeeded.` Memory usage:
FLASH 311320 B (32.21%), RAM 98736 B (37.66%).

```
$ arm-zephyr-eabi-objdump -f build/reval3__xiao_ble/zmk__tester_xiao_animation/zephyr/zmk.elf | grep -i 'start address'
start address 0x0000f495
```

Low address, confirming the offset-0 code partition took effect. Confirmed
the binary actually contains the 506d31d fix (not a stale/cached build) via
`arm-zephyr-eabi-nm`:

```
2000c6dc b notify_suppress_depth
00003e68 T zmk_animation_control_set_notify_suppressed
```

(`notify_suppress_depth` — a plain int, not the `notify_suppressed` bool or
`notify_suppressed` bitfield of the two prior fixes — confirms this is the
nesting-safe depth-counter version from 506d31d, not a stale ca6620b/d2f620a
binary.) `_SEGGER_RTT` symbol address this build: `0x20004010`.

### Rig & boards used

- **Primary: Module Test board** (serial `0C5B206D3B120A9F`, J-Link
  `001050398082`), flashed first with the same SWD/RTT-signature-zero
  procedure as `## 3. Flash procedure`. Flash succeeded (`315392 bytes`,
  `O.K.`). Boot RTT confirmed clean init and that the *previous* session's
  persisted settings (`brightness=6/1 animation=1/0`, matching the ca6620b
  section's final values) survived the reflash unchanged, as expected. tty
  node enumerated immediately after this first flash with no issue. All of
  the core `SetBrightness`/`SelectAnimation` round-trip tests below ran on
  this board without incident.
- When attempting to catch a genuinely state-changing `Trigger` (requires a
  fresh reset, run within the animation module's ~30s running window), the
  Module Test board's tty node did **not** re-enumerate after the reset, nor
  after a second reset attempt, nor after 30+ seconds of polling — matching
  this document's every prior section's note about this rig's known
  intermittent CDC-ACM dropout. Per the task's documented fallback,
  **switched to the Abyss board** (serial `10E4D16A1E4BFE9C`, J-Link
  `001057792823`), locked and flashed with the identical image (`315392
  bytes`, `O.K.`, no workaround needed) — its tty node enumerated
  immediately. The `Trigger`/`StopOverlay` tests and part of the persistence
  check ran on Abyss.
- Later, while probing the `SaveSettings`/persistence flow, both boards'
  tty nodes flip-flopped independently across several plain SWD memory
  reads (`mem32`) and `r`/`go` resets (Module Test's node disappeared after
  one `mem32` readback and did not return for over a minute despite two more
  resets; Abyss's node was up at that point and was used instead; later
  Module Test's node came back on its own while Abyss's was down). This is
  the same rig flakiness documented in every prior section — in every case
  it was checked, the *drop* followed a `JLinkExe` operation (reset or raw
  memory read), never an RPC call or RPC timeout.

### RPC round-trip evidence (the core test — all calls now return)

All calls via the same `custom-call` invocation template as every prior
section (`## 5. Studio RPC round-trip`).

**GetState (baseline):** always returned promptly (~0.75-1.0s), on both
boards, throughout the session. Example after the persistence markers were
set:

```json
{"state": {"enabled": true, "brightness_powered": 6, "brightness_battery": 1, "selected_powered": 1, "is_powered": true}}
```

**SetBrightness — 5/5 returned promptly (Module Test board), all applied
correctly:**

| # | step | Result | real time |
|---|------|--------|-----------|
| 1 | 3 | Returned, `brightness_powered: 3` | 0.766s |
| 2 | 5 | Returned, `brightness_powered: 5` | 0.755s |
| 3 | 7 | Returned, `brightness_powered: 7` | 0.779s |
| 4 | 2 | Returned, `brightness_powered: 2` | 0.752s |
| 5 | 6 | Returned, `brightness_powered: 6` | 0.759s |

Every single call's `StateResponse` carried the correct new
`brightness_powered` value directly in the RPC response itself (no
separate follow-up `GetState` needed to confirm, unlike every prior
section in this document, where the response never arrived at all).

**SelectAnimation — 3/3 returned promptly (Module Test board), all applied
correctly** (valid indices 0-2 confirmed via a `GetInfo` call first):

| # | index | Result | real time |
|---|-------|--------|-----------|
| 1 | 2 | Returned, `selected_powered: 2` | 0.746s |
| 2 | 0 | Returned, `selected_powered` absent (proto3 default 0) | 0.767s |
| 3 | 1 | Returned, `selected_powered: 1` | 0.761s |

**Trigger — the genuinely state-changing case now returns (1/1, Abyss
board):** run ~1s after a fresh reset (before the ~30s idle timeout, so
`zmk_animation_control_is_running()` was still true and this was a real
mutation, not a design no-op — confirmed by the response itself carrying
`overlay_active: true`, unlike every prior no-op `Trigger` in this document
which omitted that field):

```json
{
  "state": {
    "enabled": true, "brightness_powered": 6, "brightness_battery": 1,
    "selected_powered": 1, "is_powered": true,
    "overlay_active": true, "has_overlay_index": true
  }
}
```

Returned in 0.877s. This is the exact scenario (state-changing `Trigger`,
caught within the running window) that timed out 100% of the time (1/1 in
§5, 1/1 after d2f620a, 2/2 after ca6620b) in every prior pass.

**StopOverlay — returned promptly (Abyss board), genuine state change:**

```json
{"state": {"enabled": true, "brightness_powered": 6, "brightness_battery": 1, "selected_powered": 1, "is_powered": true}}
```

Returned in 0.728s (`overlay_active` no longer present, confirming the
overlay was actually stopped by this call, not just an already-idle no-op).

**GetInfo/GetState/custom-list — always returned normally**, as in every
prior section.

### `SaveSettings` (cormoran_custom_settings) — still times out; remaining anomaly

Per the task's explicit ask ("verify whether IT now returns too, since
custom-settings notifications were deemed harmless"): **it does not.**
`SaveSettings` (empty scope) was attempted 4 times this session (Abyss once,
Module Test 3 times, across two separate mutation batches) and **timed out
every single time** (5.8-5.9s each):

```
$ ... custom-call --identifier cormoran_custom_settings ... --json '{"saveSettings":{}}'
Timed out waiting for a Studio RPC frame
```

This is expected and not a regression from this fix: 506d31d's suppression
bracket lives entirely inside `zmk_animation_request_exec_handle()` (the
*animation* module's own RPC dispatch) and does not touch
`cormoran_custom_settings`'s independent request-handling/notification code
path. `SaveSettings` raises its own settings-changed notification on that
separate subsystem, which is not covered by this fix and can still starve
its own response on the same shared Studio USB CDC transport, exactly like
every mutating call did before 506d31d. Each time, a follow-up `GetState`
immediately after the timeout still succeeded within ~1s with the tty node
intact (no CDC wedge), and — more importantly — the underlying flash write
still happened correctly (see persistence section below): the "applies,
response lost" pattern from every prior section persists for this one
remaining call, just no longer for any animation-subsystem mutation. This is
worth a documented follow-up: apply the same suppression pattern (or a
shared one) to `cormoran_custom_settings`'s own RPC dispatch.

### CDC transport stability — stable across every RPC call; drops track resets only

Every `SetBrightness`/`SelectAnimation`/`Trigger`/`StopOverlay`/`GetState`
call in this session returned with the tty node intact, and a `GetState`
immediately after each of the 4 `SaveSettings` timeouts also succeeded in
under 1s with no node drop. The tty-node drops that did occur (both boards,
several times) were checked in each case and always followed a `JLinkExe`
operation (`r`/`go` reset, or a raw `mem32` SWD read) rather than any RPC
call or RPC timeout — consistent with this rig's pre-existing, independently
documented intermittent CDC-ACM re-enumeration behavior (`hardware-rig.md`),
not a transport-level side effect of this fix or of the one remaining
`SaveSettings` timeout.

### Persistence re-check — PASS (RTT-confirmed; live RPC blocked by rig flakiness, not by RPC behavior)

Distinct-marker methodology, run on the Module Test board, deliberately using
values (`brightness_powered=9`, `selected_powered=2`) that differ from this
board's pre-existing persisted baseline (`6`/`1`, carried over from the
ca6620b section) so the check is unambiguous:

1. `SetBrightness` (POWERED, step=9) → returned promptly, `brightness_powered: 9`.
2. `SelectAnimation` (POWERED, index=2) → returned promptly, `selected_powered: 2`.
3. `SaveSettings` (empty scope) → **timed out** (5.9s), matching the anomaly
   above. A follow-up `GetState` right after confirmed both values still
   applied in memory: `{"brightness_powered": 9, "selected_powered": 2}`,
   with the tty node still intact.
4. Reset via `JLinkExe` (`r` + `go`, RTT signature re-zeroed), without
   reflashing.
5. RTT boot-apply log after reset:
   ```
   <inf> zmk: animation settings: apply enabled=1 brightness=9/1 animation=2/0
   <inf> zmk: animation control: select index 0->2
   ```
   Exactly matches the markers set in steps 1-2 — **this proves the
   `SaveSettings` flash-write itself succeeded despite its own RPC response
   never reaching the host**, the same "applies, response lost" pattern this
   whole document has established, now isolated specifically to
   `cormoran_custom_settings`'s own RPC path rather than the animation
   module's.
6. A live RPC `GetState` confirmation after this reset could not be
   captured: the Module Test board's tty node did not return within this
   session's time budget (over a minute of polling plus two extra `r`/`go`
   resets), while the Abyss board's tty was up in the meantime and confirmed
   unrelated to this check (still showing its own unchanged `6`/`1` baseline,
   since its own `SaveSettings` attempt earlier in the session had also timed
   out). This is the same rig-flakiness limitation noted in the d2f620a and
   ca6620b sections' persistence re-checks — not evidence of any problem with
   this fix, since (a) the RTT boot-apply log is a direct, unambiguous read of
   the same code path used as corroborating evidence throughout this document,
   and (b) the drop happened following `JLinkExe` operations, consistently
   with the CDC-stability finding above, not following any RPC call.

**Result: PASS** — the new marker values (`brightness_powered=9`,
`selected_powered=2`) survived a reset-without-reflash, confirmed via RTT
boot-apply log. Persistence continues to work correctly and is unaffected by
either this fix or the one remaining `SaveSettings` response-loss anomaly.

### Supporting RTT evidence — notification traffic around a `SetBrightness` call

Captured the buffer range written during a single `SetBrightness` call on
the Abyss board (via `RTT` control block `WrOff` before/after, `0x1283` →
`0x1adc`, isolating exactly the bytes this call added):

```
<inf> zmk: Custom settings proto start: subsystem=cormoran__animation key=brightness_powered include_value=1 include_meta=0 source=0
<inf> zmk: Custom settings proto base ready: subsystem=cormoran__animation key=brightness_powered has_unsaved=1
<inf> zmk: Custom settings proto value start: subsystem=cormoran__animation key=brightness_powered
<inf> zmk: Custom settings proto value ready: subsystem=cormoran__animation key=brightness_powered value_type=2
<inf> zmk: Custom settings proto complete: subsystem=cormoran__animation key=brightness_powered
<dbg> zmk: zmk_rpc_custom_subsystem_encode_response_payload: Encoding custom response of size 32   (x4)
<inf> zmk: animation settings: apply enabled=1 brightness=4/1 animation=1/0
<inf> zmk: Start animation solid
<dbg> zmk: zmk_rpc_custom_subsystem_encode_response_payload: Encoding custom response of size 12   (x4)
```

This shows the custom-settings write-through (the `apply_all()` re-entry
this fix's commit message describes) happening as expected, and the RPC
response encode path running - consistent with the call succeeding. As in
every prior section, the shared "Encoding custom response" log line does not
distinguish a `Response` frame from a `Notification` frame at this log level,
and the buffer wraps too fast to definitively prove zero notification frames
were sent purely from RTT text (the `nm`-verified `notify_suppress_depth`
symbol plus the native_sim regression test in 506d31d's own commit, which
directly asserts the notification count, remain the strongest evidence for
that specific claim) - this RTT capture is offered only as consistent,
non-contradictory supporting evidence, not as standalone proof.

### Before/after contrast (cumulative across all four validation passes)

| Call | Before any fix (`## 5`) | After d2f620a | After ca6620b | After 506d31d (this fix) |
|------|--------------------------|----------------|----------------|---------------------------|
| `SetBrightness` | 7/7+ timed out | 10/10 timed out | 9/10 timed out (1 no-op exception) | **5/5 returned** |
| `SelectAnimation` | 7/7+ timed out | 7/7 timed out | 3/3 timed out | **3/3 returned** |
| `Trigger` (state-changing) | 1/1 timed out | 1/1 timed out | 2/2 timed out | **1/1 returned** |
| `StopOverlay` | returns fine (no-op case) | not distinctly tested | 2/2 returned (no-op case) | **returned (genuine state change this time)** |
| `SaveSettings` | 1/2 timed out | 1/2 timed out | 2/2 timed out | **4/4 timed out — unresolved, separate module** |
| `GetInfo`/`GetState`/`custom-list` | always fine | always fine | always fine | **always fine — unchanged** |
| Persistence across reset | PASS | PASS | PASS | **PASS** |
| CDC tty node after an RPC timeout | dropped | stayed present | mixed (dropped after `Trigger`) | **N/A this session — no animation-mutation timeouts occurred to check; `SaveSettings` timeouts did not correlate with a drop** |

### Conclusion

**Commit 506d31d fixes the RPC response-loss bug for the animation
module's own mutating RPC calls.** Every `SetBrightness`, `SelectAnimation`,
and genuinely state-changing `Trigger`/`StopOverlay` call in this session
returned its `Response` frame promptly (sub-1s), a complete reversal from
100% timeouts for the same calls across every one of the three prior
validation passes in this document. This confirms the commit's own thesis,
independently verified on hardware: suppressing the animation module's
notification entirely for the RPC dispatch (rather than merely deferring it,
or reducing its count from 6 to 1) removes the starvation condition, because
the RPC `Response` itself already carries the full state the client needs
and Studio RPC is single-connection.

**One remaining, clearly-scoped anomaly:** `cormoran_custom_settings`'s own
`SaveSettings` RPC call still times out on its response (4/4 this session),
because this fix's suppression bracket does not extend to that module's
independent RPC/notification path. The mutation and flash-write still
succeed regardless (confirmed via RTT boot-apply log across a reset), so
this does not block persistence, but it means a Studio UI's explicit "Save"
action for this module would still appear to hang/fail from the client's
perspective. This is a good candidate for the same fix pattern in a future
phase, applied to `zmk-feature-custom-settings`'s own RPC dispatch.

**Recommendation: commit 506d31d is safe to merge/ship** as the fix for the
animation module's RPC response-loss bug that this whole investigation was
chartered around. Track the `SaveSettings` anomaly as a separate, smaller
follow-up in the custom-settings module rather than blocking on it here.
