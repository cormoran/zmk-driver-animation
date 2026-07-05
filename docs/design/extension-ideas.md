# Extension ideas (post-v2.0 backlog)

Not scheduled; each item notes why it was deferred and what it builds on.
Ordered roughly by value/effort.

## Runtime animation parameters over RPC

Expose per-animation-instance parameters (solid colors, cycle duration,
battery thresholds, endpoint colors) as writable values — either as extra
`zmk_custom_setting` entries generated per DT instance (gets persistence +
generic UI for free, but the registry grows fast) or as a dedicated
`SetParam` RPC with a per-animation param schema in `GetInfoResponse`.
Deferred from v2.0 to keep the proto small; the `control.h`/`request_exec`
split already leaves room. Start with `animation-solid` colors as the pilot.

## Reactive / ripple animations (revive the pixel-distance LUT)

v1 shipped a precomputed pairwise pixel-distance LUT and a `key-pixels` map
with zero consumers (and a leftover `animation_ripple_` identifier hinting at
the original plan). A `zmk,animation-ripple` reacting to
`zmk_position_state_changed` (expanding ring from the pressed key, using the
LUT) plus a `zmk,animation-reactive` (light the pressed key's pixel) would
make the LUT earn its RAM. Gate the LUT behind the animation types that use
it instead of default-y.

## Real blending compositor

v1 declared 5 blend modes and a `blending-mode` DT property that did nothing.
If overlays should mix with the base (e.g. translucent battery bar over the
base animation), implement blending at the §3.3 render pipeline's overlay
step, where there is exactly one place that composites — per-overlay
`blend-mode` + alpha. Only worth it with a concrete visual use case.

## Split-peripheral RPC relay

Adopt pmw3610's relay architecture (`RelayRequest`/`RelayResponse` over
`CONFIG_ZMK_SPLIT_RELAY_EVENT`, `source` field on requests, peripheral
executes via the same `request_exec`) so the web UI can control/inspect a
peripheral half's strip, and replace the `animls` behavior-invocation hack
with a proper relay message. This is the main reason `request_exec.c` is
transport-agnostic. Mind the 255-byte relay payload cap when sizing protos.

## Playlist / scheduler animations

Time-of-day or rotation playlists (cycle the base animation every N minutes,
dim after HH:MM). Needs a wall-clock source (host-synced via a small RPC —
Studio connection could push time) — without one, only uptime-relative
scheduling is honest.

## Host-driven pixel streaming

A `SetPixels` RPC (or notification-rate-limited stream) letting the host
paint the strip directly — music visualizers, CI status, notification
flashes. Needs chunking within RX buffer limits and an auto-revert timeout
back to the selected animation. Security: keep it behind
`ZMK_STUDIO_RPC_HANDLER_SECURED` or a Kconfig, since it's an unbounded-ish
write surface.

## OS-/endpoint-aware theming

Combine with `zmk-feature-os-detection`: auto-select animation/color per
detected host OS or per BLE profile (e.g. blue on macOS profile, orange on
Windows). Cheap once runtime parameters exist.

## Battery-aware brightness curve

Replace the binary powered/battery brightness pair with an optional curve
(brightness derated as state-of-charge drops, plus a hard floor). v1's
ext-power gating already saves idle power; this addresses active drain.

## Multiple engine instances / independent strips

v1 and v2.0 hardcode a single engine (one pixel buffer, one tick). Per-strip
engines (underglow vs per-key at different FPS) would need the engine's
singletons (timer/work/buffer) instance-scoped — mostly mechanical after the
v2 core split, but no known user demand yet.

## Animation "bytecode" upload

Let the web UI upload tiny interpreted animation programs (keyframes or a
small op set) to flash via custom settings. Big feature: needs storage
budgeting, validation, and a safe interpreter; only worth exploring if
predefined types prove too limiting.

## Studio keymap integration

Layer-status colors sourced from Studio keymap metadata (layer names/colors)
instead of DT arrays, so the same tool that edits layers styles their
indicator.
