/**
 * @blacknode/irc-client -- a connection, the state it carries, and the
 * conversation on top of it.
 *
 * The same code on every target.  What differs is the transport, and
 * that is an interface with four methods:
 *
 *   web (Next.js on Bun)   `WebSocketTransport`, usually inside the
 *                          SharedWorker in `./worker`, so that ten tabs
 *                          are one connection and not ten
 *   mobile (React Native)  `WebSocketTransport` directly; there is one
 *                          JavaScript context, so there is nothing to
 *                          share a connection between
 *   desktop (Wails)        `WebSocketTransport`; the frontend is a
 *                          webview, so it is the browser's WebSocket
 *
 * Nothing here imports `window`, `document`, `process` or `Buffer`.
 */

export { Client, type ClientOptions, type ClientStatus, type SendOptions } from './client.js';

export {
  type ClientEvent,
  type ClientListener,
  type DisconnectReason,
  Emitter,
} from './events.js';

export {
  NetworkState,
  type Channel,
  type Membership,
  type StoredMessage,
  type User,
} from './state.js';

export {
  type Transport,
  type TransportHandlers,
  type WebSocketLike,
  type WebSocketOptions,
  WebSocketTransport,
} from './transport.js';

// Re-exported so a caller needs one import for the ordinary case.
export {
  type ChatHistorySelector,
  type Credential,
  type Message,
  type StandardReply,
  TAG_FORMAT,
  TAG_REACT,
  TAG_REPLY,
  TAG_TYPING,
  FORMAT_MARKDOWN,
} from '@blacknode/irc-protocol';
