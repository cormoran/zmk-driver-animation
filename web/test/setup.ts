// jest-dom adds custom jest matchers for asserting on DOM nodes.
import "@testing-library/jest-dom";

// jsdom's global scope does not include TextEncoder/TextDecoder (a known
// gap: https://github.com/jsdom/jsdom/issues/2524), but @bufbuild/protobuf
// (used by the generated animation proto bindings) needs them to
// encode/decode messages. Node itself provides both via the "util" module,
// so polyfill the globals from there for tests that encode/decode proto
// messages directly (e.g. simulating a state_changed notification).
import { TextDecoder, TextEncoder } from "util";

if (typeof globalThis.TextEncoder === "undefined") {
  // @ts-expect-error -- Node's TextEncoder/TextDecoder types are close
  // enough to the DOM lib types for this test-only polyfill.
  globalThis.TextEncoder = TextEncoder;
}
if (typeof globalThis.TextDecoder === "undefined") {
  // @ts-expect-error -- see above.
  globalThis.TextDecoder = TextDecoder;
}
