# zmk-driver-animation — Web UI

A [ZMK Studio](https://zmk.dev/docs/features/studio) web app for controlling
the `zmk-driver-animation` LED animation module live over its custom Studio RPC
subsystem (`cormoran__animation`). Connect a keyboard running animation
firmware and adjust it from the browser — no reflash needed.

## Features

- **Device connection**: Connects to the keyboard over **Serial** (WebSerial)
  through ZMK Studio. (BLE is not wired up in this app.)
- **Live control**: View current state and set per-power-source (USB / battery)
  brightness and base-animation selection, trigger overlay animations, and stop
  the active overlay.
- **Live updates**: Subscribes to the module's `state_changed` notification, so
  changes made elsewhere (a keymap behavior, power-source switch) are reflected
  without polling.
- Built with **React + TypeScript + Vite** on the
  [`@cormoran/zmk-studio-react-hook`](https://github.com/cormoran/react-zmk-studio)
  library.

The firmware must be built with `CONFIG_ZMK_STUDIO=y` and
`CONFIG_ZMK_ANIMATION_STUDIO_RPC=y` (see the repo's top-level `README.md`).

## Quick start

```bash
# Install dependencies
npm ci

# Generate the TypeScript protobuf types from ../proto (required before first run)
npm run generate

# Run the dev server
npm run dev

# Build for production
npm run build

# Run tests / lint
npm test
npm run lint
```

Open the dev server, click **Connect Serial**, and pick the keyboard's serial
port when prompted.

## Project structure

```
src/
├── main.tsx              # React entry point
├── App.tsx               # Connection UI (ZMKConnection, serial transport)
├── AnimationPanel.tsx    # The animation control panel (state, brightness, pickers, triggers)
├── App.css               # Styles
└── proto/                # Generated protobuf TypeScript types
    └── cormoran/animation/animation.ts

test/                     # Component + RPC tests
```

## How it works

### Protocol

The protobuf schema lives in `../proto/cormoran/animation/animation.proto`
(shared with the firmware). `npm run generate` runs `buf generate` (config in
`buf.gen.yaml`) to produce the TypeScript types under `src/proto/`.

### Talking to the firmware

The app uses `@cormoran/zmk-studio-react-hook` to connect and find the
animation subsystem by its identifier, then makes typed RPC calls:

```typescript
import { useZMKApp, ZMKCustomSubsystem } from "@cormoran/zmk-studio-react-hook";

const { state, findSubsystem, isConnected } = useZMKApp();
const subsystem = findSubsystem("cormoran__animation");
const service = new ZMKCustomSubsystem(state.connection, subsystem.index);
const response = await service.callRPC(payload);
```

## Testing

```bash
npm test                  # run once
npm run test:watch        # watch mode
npm run test:coverage     # with coverage
```

Tests use the mock helpers from `@cormoran/zmk-studio-react-hook/testing`
(`createConnectedMockZMKApp`, `ZMKAppProvider`) so components can be exercised
without real hardware.
