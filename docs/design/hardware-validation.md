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
