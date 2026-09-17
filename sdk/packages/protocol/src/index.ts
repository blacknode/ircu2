/**
 * @blacknode/irc-protocol -- the wire, and nothing else.
 *
 * Pure: no sockets, no timers, no globals, no runtime dependencies.  It
 * runs unchanged in a browser, in React Native, in a Wails webview and
 * under Bun, because there is nothing in it that could tell them apart.
 * Everything that has to touch a socket is in `@blacknode/irc-client`.
 *
 * That split is the same one the server makes between `ircd/sasl.c` and
 * `ircd/m_authenticate.c`, or `ircd/migration.c` and
 * `ircd/migration_run.c`, and for the same reason: the half that decides
 * things can then be tested exhaustively, and it is the half where being
 * wrong is expensive.
 */

export {
  type Tags,
  type Prefix,
  type Message,
  type OutgoingMessage,
  MAX_BODY_BYTES,
  MAX_CLIENT_TAG_BYTES,
  parseMessage,
  formatMessage,
  escapeTagValue,
  unescapeTagValue,
  byteLength,
  splitForLine,
} from './message.js';

export {
  WANTED_CAPS,
  type WantedCap,
  type CapStep,
  CapState,
} from './caps.js';

export {
  type Credential,
  type SaslOutcome,
  type SaslStep,
  SaslSession,
  chunkPayload,
  toBase64,
  fromBase64,
} from './sasl.js';

export {
  type OpenBatch,
  type BatchEvent,
  BatchAssembler,
  joinMultiline,
  splitMultiline,
} from './batch.js';

export {
  TAG_REPLY,
  TAG_REACT,
  TAG_TYPING,
  TAG_FORMAT,
  FORMAT_MARKDOWN,
  TAG_EDITED,
  type StandardReply,
  parseStandardReply,
  type ChatHistorySelector,
  chatHistoryCommand,
  type SearchOptions,
  searchCommand,
  editCommand,
} from './conversation.js';
