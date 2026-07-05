import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import { ZMKConnection } from "@cormoran/zmk-studio-react-hook";
import { AnimationPanel } from "./AnimationPanel";

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>zmk-driver-animation</h1>
        <p>LED animation control (custom Studio RPC)</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>{error}</p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <AnimationPanel />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>zmk-driver-animation</strong> - LED animation state,
          brightness, animation selection, and triggers over custom Studio RPC
        </p>
      </footer>
    </div>
  );
}

export default App;
