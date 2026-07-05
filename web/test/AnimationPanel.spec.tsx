import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import {
  AnimationPanel,
  ANIMATION_SUBSYSTEM_IDENTIFIER,
} from "../src/AnimationPanel";
import {
  GetInfoResponse,
  Notification as AnimationNotification,
  Request,
  Response,
  StateResponse,
} from "../src/proto/cormoran/animation/animation";

const INFO: GetInfoResponse = GetInfoResponse.create({
  poweredAnimations: [
    { index: 0, name: "Solid Red" },
    { index: 1, name: "Rainbow" },
  ],
  batteryAnimations: [{ index: 0, name: "Solid Blue" }],
  behaviorAnimations: [{ index: 0, name: "Sparkle" }],
  numPixels: 16,
  fps: 30,
  brightnessSteps: 5,
});

const STATE: StateResponse = StateResponse.create({
  enabled: true,
  brightnessPowered: 3,
  brightnessBattery: 2,
  selectedPowered: 0,
  selectedBattery: 0,
  isPowered: true,
  overlayActive: false,
  hasOverlayIndex: false,
  overlayIndex: 0,
});

/**
 * Wires a mock ZMKCustomSubsystem.callRPC-equivalent by mocking the
 * connection's RPC transport at the level this component actually calls:
 * `ZMKCustomSubsystem` wraps `connection` from `zmkApp.state.connection`, so
 * we mock the module directly rather than reaching through the transport.
 */
function mockAnimationRpc(handler: (request: Request) => Response) {
  jest
    .spyOn(
      // eslint-disable-next-line @typescript-eslint/no-require-imports
      require("@cormoran/zmk-studio-react-hook"),
      "ZMKCustomSubsystem"
    )
    .mockImplementation(function (this: unknown) {
      return {
        callRPC: async (payload: Uint8Array) => {
          const request = Request.decode(payload);
          const response = handler(request);
          return Response.encode(response).finish();
        },
      };
    });
}

jest.mock("@cormoran/zmk-studio-react-hook", () => {
  const actual = jest.requireActual("@cormoran/zmk-studio-react-hook");
  return {
    ...actual,
    ZMKCustomSubsystem: jest.fn(),
  };
});

describe("AnimationPanel Component", () => {
  afterEach(() => {
    jest.restoreAllMocks();
  });

  describe("Without the subsystem", () => {
    it("shows a warning naming the subsystem identifier", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <AnimationPanel />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(
          new RegExp(
            `Subsystem "${ANIMATION_SUBSYSTEM_IDENTIFIER}" not found`,
            "i"
          )
        )
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<AnimationPanel />);
      expect(container.firstChild).toBeNull();
    });
  });

  describe("With the subsystem", () => {
    it("loads info + state and renders the control panel", async () => {
      mockAnimationRpc((request) => {
        if (request.getInfo) return Response.create({ info: INFO });
        if (request.getState) return Response.create({ state: STATE });
        throw new Error("unexpected request");
      });

      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [ANIMATION_SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <AnimationPanel />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByText("Solid Red")).toBeInTheDocument();
      });

      expect(screen.getByText("Rainbow")).toBeInTheDocument();
      expect(screen.getByText("Solid Blue")).toBeInTheDocument();
      expect(screen.getByText("Sparkle")).toBeInTheDocument();
      expect(screen.getByText(/Powered \(USB\)/i)).toBeInTheDocument();
      expect(screen.getByText("none")).toBeInTheDocument();

      const enabledToggle = screen.getByLabelText(
        /Enabled/i
      ) as HTMLInputElement;
      expect(enabledToggle.checked).toBe(true);
    });

    it("sends a SelectAnimationRequest when an animation button is clicked", async () => {
      let lastRequest: Request | null = null;
      mockAnimationRpc((request) => {
        if (request.getInfo) return Response.create({ info: INFO });
        if (request.getState) return Response.create({ state: STATE });
        lastRequest = request;
        return Response.create({
          state: StateResponse.create({ ...STATE, selectedPowered: 1 }),
        });
      });

      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [ANIMATION_SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <AnimationPanel />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByText("Rainbow")).toBeInTheDocument();
      });

      const user = userEvent.setup();
      await user.click(screen.getByText("Rainbow"));

      await waitFor(() => {
        expect(lastRequest?.selectAnimation).toBeDefined();
      });
      expect(lastRequest?.selectAnimation?.index).toBe(1);
    });

    it("updates state when a state_changed notification arrives", async () => {
      mockAnimationRpc((request) => {
        if (request.getInfo) return Response.create({ info: INFO });
        if (request.getState) return Response.create({ state: STATE });
        throw new Error("unexpected request");
      });

      let capturedCallback:
        | ((notification: { payload: Uint8Array }) => void)
        | null = null;
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [ANIMATION_SUBSYSTEM_IDENTIFIER],
      });
      mockZMKApp.onNotification = jest.fn((subscription) => {
        if (subscription.type === "custom") {
          capturedCallback = subscription.callback;
        }
        return () => {};
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <AnimationPanel />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByText(/Powered \(USB\)/i)).toBeInTheDocument();
      });
      expect(capturedCallback).not.toBeNull();

      // Simulate a state change pushed from firmware (e.g. keymap unplugged
      // USB), not triggered by any RPC call this UI made.
      const pushedState = StateResponse.create({
        ...STATE,
        isPowered: false,
        overlayActive: true,
        hasOverlayIndex: true,
        overlayIndex: 0,
      });
      const payload = AnimationNotification.encode(
        AnimationNotification.create({ stateChanged: pushedState })
      ).finish();

      capturedCallback!({ payload });

      await waitFor(() => {
        expect(screen.getByText(/^Battery$/i)).toBeInTheDocument();
      });
      expect(screen.getAllByText("Sparkle").length).toBeGreaterThan(0);
      expect(screen.getByText(/Stop overlay/i)).toBeInTheDocument();
    });
  });
});
