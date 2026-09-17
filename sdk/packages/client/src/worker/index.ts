/**
 * `@blacknode/irc-client/worker` -- one connection for however many tabs.
 *
 * A person with eight tabs open is one person.  Eight connections would
 * be eight clients flooding the server, eight copies of the same history
 * and eight notifications for one message, so the connection lives in a
 * worker and the tabs talk to it over a port.
 *
 * `WorkerHub` is all of the decisions and none of the platform: it takes
 * a `PortLike`, which a `SharedWorker` port, a `Worker` and a test fake
 * all are.  `shared-worker.js` is the entry point that gives it a real
 * one, and is imported for its side effect rather than from here.
 */

export {
  WorkerHub,
  mentions,
  type HubOptions,
  type PortLike,
  type TransportFactory,
} from './hub.js';

export {
  type FromWorker,
  type ToWorker,
  type WorkerConnectRequest,
  type WorkerSnapshot,
} from './protocol.js';
