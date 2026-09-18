/**
 * What the SDK assumes a runtime gives it -- the whole list.
 *
 * "DOM" is deliberately not in `lib`: the SDK runs in a browser, in
 * React Native, in a Wails webview and under Bun, and the surest way to
 * keep it portable is to keep the browser's types out of scope, so that
 * a stray `window` or `document` is a compile error here rather than a
 * crash on somebody's phone.
 *
 * So this file is the contract, and it is four things: the two text
 * codecs, the timers, and `WebSocket`.  Every one of them is in all four
 * runtimes.  Anything a runtime does *not* have -- `SharedWorker`,
 * `Notification`, `localStorage` -- is reached through an interface the
 * caller implements, never a global.
 */

declare class TextEncoder {
  encode(input?: string): Uint8Array;
  readonly encoding: string;
}

declare class TextDecoder {
  constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });
  decode(input?: ArrayBuffer | ArrayBufferView): string;
  readonly encoding: string;
}

declare function setTimeout(handler: () => void, timeout?: number): unknown;
declare function clearTimeout(handle: unknown): void;
