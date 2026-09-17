/**
 * The twenty lines that turn `WorkerHub` into an actual SharedWorker.
 *
 * Everything that decides anything is in `hub.ts`, which has no `self`
 * in it and can therefore be tested.  This file is the part that cannot
 * be: it exists to be bundled as a worker entry point.
 *
 *   // app/irc.worker.ts
 *   import '@blacknode/irc-client/worker/shared-worker';
 *
 *   // in a tab
 *   const worker = new SharedWorker(new URL('./irc.worker.ts', import.meta.url),
 *                                   { type: 'module' });
 *   worker.port.start();
 *
 * A SharedWorker is not available in every browser -- Chrome on Android
 * has none -- and `connectHub()` below is deliberately separate so the
 * same hub can be driven from a plain `Worker`, or from nothing at all
 * on a runtime that has neither.  See doc/readme.sdk.
 */

import { WebSocketTransport } from '../transport.js';
import { WorkerHub, type PortLike } from './hub.js';

/** The hub this worker runs.  One per worker, which is one per origin. */
export const hub = new WorkerHub({
  transportFactory: (url) => new WebSocketTransport({ url }),
});

/** Attach a port.  Exported so a plain Worker can call it too. */
export function connectHub(port: PortLike): void {
  hub.attach(port);
}

// A SharedWorker gets `onconnect`; a dedicated Worker gets messages on
// its own global.  Both are supported, because "which kind of worker"
// is a decision about the platform and not about the protocol.
const scope = globalThis as unknown as {
  onconnect?: ((ev: { ports: PortLike[] }) => void) | null;
  postMessage?: (value: unknown) => void;
  addEventListener?: (type: string, fn: (ev: unknown) => void) => void;
};

if ('onconnect' in scope) {
  scope.onconnect = (ev) => {
    const port = ev.ports[0];

    if (port) connectHub(port);
  };
} else if (typeof scope.postMessage === 'function' && scope.addEventListener) {
  // A dedicated worker: the global *is* the port.
  connectHub({
    postMessage: (value) => scope.postMessage?.(value),
    onmessage: null,
    start: () => {
      scope.addEventListener?.('message', (ev) => {
        const self = hubPort;

        self.onmessage?.(ev as { data: unknown });
      });
    },
  });
}

/** Kept so the dedicated-worker path has something to deliver to. */
const hubPort: PortLike = {
  postMessage: (value) => scope.postMessage?.(value),
  onmessage: null,
};
