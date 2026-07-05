import { useContext, useEffect, useState } from "react";
import {
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  AnimationInfo,
  GetInfoResponse,
  Notification as AnimationNotification,
  PowerSource,
  Request,
  Response,
  StateResponse,
  TriggerMode,
} from "./proto/cormoran/animation/animation";

/**
 * Animation control panel for the "cormoran__animation" custom Studio RPC
 * subsystem (DESIGN.md #3.6): state display, brightness sliders, per-source
 * animation pickers, behavior-animation trigger buttons, and a stop-overlay
 * button.
 *
 * Live updates: this component subscribes to
 * `zmkApp.onNotification({type: "custom", subsystemIndex, callback})`
 * (`@cormoran/zmk-studio-react-hook`'s real push-notification API -- see
 * useZMKApp's `NotificationSubscription` type and how
 * zmk-driver-pmw3610-with-custom-studio-rpc's FrameViewer/useStudioLockState
 * use it) and decodes `Notification.state_changed`. This is a genuine
 * server-push mechanism, not a poll: no polling fallback was needed.
 */
export const ANIMATION_SUBSYSTEM_IDENTIFIER = "cormoran__animation";

// Trigger defaults for the behavior-animations buttons (DESIGN.md #3.6 /
// Phase D scope note): a short, cancelable, immediate overlay is the most
// useful one-click default for a web UI "try this animation" button.
// Exposing duration/mode/cancelable as UI controls was judged not worth the
// extra surface for this phase -- these are fixed, documented constants.
const TRIGGER_DURATION_MS = 3000;
const TRIGGER_MODE: TriggerMode = TriggerMode.TRIGGER_MODE_PLAY_NOW;
const TRIGGER_CANCELABLE = true;

export interface AnimationPanelProps {
  /** True while ZMK Studio is locked -- disables write controls. Optional:
   * this module's RPC subsystem is unsecured (DESIGN.md #3.6), so this is
   * only ever passed by a caller that also gates other secured subsystems
   * on the same page and wants consistent disabling. */
  locked?: boolean;
}

export function AnimationPanel({ locked = false }: AnimationPanelProps = {}) {
  const zmkApp = useContext(ZMKAppContext);
  const subsystem = zmkApp?.findSubsystem(ANIMATION_SUBSYSTEM_IDENTIFIER);

  const [info, setInfo] = useState<GetInfoResponse | null>(null);
  const [state, setState] = useState<StateResponse | null>(null);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const callRequest = async (request: Request): Promise<Response> => {
    const connection = zmkApp?.state.connection;
    if (!connection || !subsystem) {
      throw new Error("Animation subsystem is not available");
    }
    const service = new ZMKCustomSubsystem(connection, subsystem.index);
    const payload = Request.encode(request).finish();
    const responsePayload = await service.callRPC(payload);
    if (!responsePayload) {
      throw new Error("Empty response");
    }
    return Response.decode(responsePayload);
  };

  const refreshInfo = async () => {
    const resp = await callRequest(Request.create({ getInfo: {} }));
    if (resp.error) {
      throw new Error(resp.error.message);
    }
    if (!resp.info) {
      throw new Error("GetInfo response missing info field");
    }
    setInfo(resp.info);
  };

  const refreshState = async () => {
    const resp = await callRequest(Request.create({ getState: {} }));
    if (resp.error) {
      throw new Error(resp.error.message);
    }
    if (!resp.state) {
      throw new Error("GetState response missing state field");
    }
    setState(resp.state);
  };

  const loadAll = async () => {
    setIsLoading(true);
    setError(null);
    try {
      await refreshInfo();
      await refreshState();
    } catch (e) {
      setError(e instanceof Error ? e.message : "Unknown error");
    } finally {
      setIsLoading(false);
    }
  };

  // Initial load once the subsystem becomes available.
  useEffect(() => {
    // Fire-and-forget: loadAll manages its own loading/error state.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    void loadAll();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem?.index]);

  // Live updates: the firmware pushes Notification.state_changed whenever
  // control state changes for any reason (RPC, keymap behavior, settings
  // restore, power-source change, ...) -- DESIGN.md #3.6. Subscribing here
  // keeps the panel live even when state changes did not originate from
  // this UI's own RPC calls, which is the whole point of wiring
  // notifications instead of only updating state after our own requests.
  useEffect(() => {
    if (!zmkApp || !subsystem) return;
    const unsubscribe = zmkApp.onNotification({
      type: "custom",
      subsystemIndex: subsystem.index,
      callback: (notification) => {
        let decoded: AnimationNotification;
        try {
          decoded = AnimationNotification.decode(notification.payload);
        } catch {
          return;
        }
        if (decoded.stateChanged) {
          setState(decoded.stateChanged);
        }
      },
    });
    return unsubscribe;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [zmkApp, subsystem?.index]);

  const runMutation = async (request: Request) => {
    setIsLoading(true);
    setError(null);
    try {
      const resp = await callRequest(request);
      if (resp.error) {
        throw new Error(resp.error.message);
      }
      if (resp.state) {
        // Idempotent read-back (DESIGN.md #3.6): every Set*/Select/Trigger/
        // StopOverlay response already carries the full new state, so this
        // does not need to wait for the notification to arrive.
        setState(resp.state);
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : "Unknown error");
    } finally {
      setIsLoading(false);
    }
  };

  const setEnabled = (enabled: boolean) =>
    void runMutation(Request.create({ setEnabled: { enabled } }));

  const setBrightness = (powerSource: PowerSource, step: number) =>
    void runMutation(Request.create({ setBrightness: { powerSource, step } }));

  const selectAnimation = (powerSource: PowerSource, index: number) =>
    void runMutation(
      Request.create({ selectAnimation: { powerSource, index } })
    );

  const triggerAnimation = (index: number) =>
    void runMutation(
      Request.create({
        trigger: {
          index,
          durationMs: TRIGGER_DURATION_MS,
          mode: TRIGGER_MODE,
          cancelable: TRIGGER_CANCELABLE,
        },
      })
    );

  const stopOverlay = () =>
    void runMutation(Request.create({ stopOverlay: {} }));

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <h2>Animation</h2>
        <div className="warning-message">
          <p>
            Subsystem &quot;{ANIMATION_SUBSYSTEM_IDENTIFIER}&quot; not found.
            Make sure your firmware includes the animation module with
            CONFIG_ZMK_ANIMATION_STUDIO_RPC=y.
          </p>
        </div>
      </section>
    );
  }

  if (!info || info.poweredAnimations.length === 0) {
    return (
      <section className="card">
        <h2>Animation</h2>
        {error && (
          <div className="error-message">
            <p>{error}</p>
          </div>
        )}
        <p className="empty-message">
          {isLoading
            ? "Loading animation info..."
            : "No animations configured on this device, or info could not be loaded."}
        </p>
        <div className="toolbar">
          <button
            className="btn"
            disabled={isLoading || locked}
            onClick={() => void loadAll()}
          >
            Refresh
          </button>
        </div>
      </section>
    );
  }

  const overlayName = overlayDisplayName(state, info);

  return (
    <section className="card">
      <h2>Animation</h2>

      {error && (
        <div className="error-message">
          <p>{error}</p>
        </div>
      )}

      <StatePanel
        state={state}
        info={info}
        overlayName={overlayName}
        isLoading={isLoading}
        locked={locked}
        onToggleEnabled={setEnabled}
        onRefresh={() => void loadAll()}
        onStopOverlay={stopOverlay}
      />

      <BrightnessControls
        state={state}
        brightnessSteps={info.brightnessSteps}
        isLoading={isLoading}
        locked={locked}
        onChange={setBrightness}
      />

      <AnimationPicker
        title="Powered animation"
        animations={info.poweredAnimations}
        selectedIndex={state?.selectedPowered}
        isLoading={isLoading}
        locked={locked}
        onSelect={(index) =>
          selectAnimation(PowerSource.POWER_SOURCE_POWERED, index)
        }
      />

      <AnimationPicker
        title="Battery animation"
        animations={info.batteryAnimations}
        selectedIndex={state?.selectedBattery}
        isLoading={isLoading}
        locked={locked}
        onSelect={(index) =>
          selectAnimation(PowerSource.POWER_SOURCE_BATTERY, index)
        }
      />

      <TriggerButtons
        animations={info.behaviorAnimations}
        isLoading={isLoading}
        locked={locked}
        onTrigger={triggerAnimation}
      />

      <dl className="setting-summary">
        <div>
          <dt>Pixels</dt>
          <dd>{info.numPixels}</dd>
        </div>
        <div>
          <dt>FPS</dt>
          <dd>{info.fps}</dd>
        </div>
        <div>
          <dt>Brightness steps</dt>
          <dd>{info.brightnessSteps}</dd>
        </div>
      </dl>
    </section>
  );
}

function overlayDisplayName(
  state: StateResponse | null,
  info: GetInfoResponse
): string | null {
  if (!state?.overlayActive) return null;
  if (!state.hasOverlayIndex) return "(unnamed overlay)";
  const match = info.behaviorAnimations.find(
    (a) => a.index === state.overlayIndex
  );
  return match ? match.name : `#${state.overlayIndex}`;
}

interface StatePanelProps {
  state: StateResponse | null;
  info: GetInfoResponse;
  overlayName: string | null;
  isLoading: boolean;
  locked: boolean;
  onToggleEnabled: (enabled: boolean) => void;
  onRefresh: () => void;
  onStopOverlay: () => void;
}

function StatePanel({
  state,
  overlayName,
  isLoading,
  locked,
  onToggleEnabled,
  onRefresh,
  onStopOverlay,
}: StatePanelProps) {
  return (
    <div className="form-grid">
      <label htmlFor="enabled-toggle">Enabled</label>
      <input
        id="enabled-toggle"
        type="checkbox"
        checked={state?.enabled ?? false}
        disabled={isLoading || locked || !state}
        onChange={(e) => onToggleEnabled(e.target.checked)}
      />

      <span className="form-label">Power source</span>
      <span>
        {state ? (state.isPowered ? "Powered (USB)" : "Battery") : "-"}
      </span>

      <span className="form-label">Active overlay</span>
      <span>
        {overlayName ? (
          <>
            {overlayName}{" "}
            <button
              className="btn btn-danger"
              disabled={isLoading || locked}
              onClick={onStopOverlay}
            >
              Stop overlay
            </button>
          </>
        ) : (
          "none"
        )}
      </span>
      <span className="form-label">&nbsp;</span>
      <button
        className="btn"
        disabled={isLoading || locked}
        onClick={onRefresh}
      >
        Refresh
      </button>
    </div>
  );
}

interface BrightnessControlsProps {
  state: StateResponse | null;
  brightnessSteps: number;
  isLoading: boolean;
  locked: boolean;
  onChange: (powerSource: PowerSource, step: number) => void;
}

function BrightnessControls({
  state,
  brightnessSteps,
  isLoading,
  locked,
  onChange,
}: BrightnessControlsProps) {
  return (
    <div className="form-grid">
      <label htmlFor="brightness-powered">
        Brightness (powered): {state?.brightnessPowered ?? "-"}
      </label>
      <input
        id="brightness-powered"
        type="range"
        min={0}
        max={brightnessSteps}
        value={state?.brightnessPowered ?? 0}
        disabled={isLoading || locked || !state}
        onChange={(e) =>
          onChange(PowerSource.POWER_SOURCE_POWERED, Number(e.target.value))
        }
      />

      <label htmlFor="brightness-battery">
        Brightness (battery): {state?.brightnessBattery ?? "-"}
      </label>
      <input
        id="brightness-battery"
        type="range"
        min={0}
        max={brightnessSteps}
        value={state?.brightnessBattery ?? 0}
        disabled={isLoading || locked || !state}
        onChange={(e) =>
          onChange(PowerSource.POWER_SOURCE_BATTERY, Number(e.target.value))
        }
      />
    </div>
  );
}

interface AnimationPickerProps {
  title: string;
  animations: AnimationInfo[];
  selectedIndex: number | undefined;
  isLoading: boolean;
  locked: boolean;
  onSelect: (index: number) => void;
}

function AnimationPicker({
  title,
  animations,
  selectedIndex,
  isLoading,
  locked,
  onSelect,
}: AnimationPickerProps) {
  return (
    <div className="picker-group">
      <span className="form-label">{title}</span>
      <div className="toolbar">
        {animations.map((animation) => (
          <button
            key={animation.index}
            className={
              animation.index === selectedIndex ? "btn btn-primary" : "btn"
            }
            disabled={isLoading || locked}
            aria-pressed={animation.index === selectedIndex}
            onClick={() => onSelect(animation.index)}
          >
            {animation.name}
          </button>
        ))}
        {animations.length === 0 && (
          <span className="empty-message">None configured</span>
        )}
      </div>
    </div>
  );
}

interface TriggerButtonsProps {
  animations: AnimationInfo[];
  isLoading: boolean;
  locked: boolean;
  onTrigger: (index: number) => void;
}

function TriggerButtons({
  animations,
  isLoading,
  locked,
  onTrigger,
}: TriggerButtonsProps) {
  return (
    <div className="picker-group">
      <span className="form-label">
        Trigger ({TRIGGER_DURATION_MS / 1000}s, play now, cancelable)
      </span>
      <div className="toolbar">
        {animations.map((animation) => (
          <button
            key={animation.index}
            className="btn"
            disabled={isLoading || locked}
            onClick={() => onTrigger(animation.index)}
          >
            {animation.name}
          </button>
        ))}
        {animations.length === 0 && (
          <span className="empty-message">None configured</span>
        )}
      </div>
    </div>
  );
}
