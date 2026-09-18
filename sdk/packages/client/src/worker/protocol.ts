/**
 * What a tab and the shared worker say to each other.
 *
 * The web client's shape, and the reason there is a worker at all: a
 * person with eight tabs open is **one** person with one connection, not
 * eight clients flooding the server and eight copies of the same
 * history.  The worker holds the connection; the tabs hold the screen.
 *
 * That also gives notifications for free, which is what phase 6 asked
 * for before any push infrastructure exists: the worker outlives every
 * tab's render and keeps receiving while a tab is in the background, so
 * "you were mentioned" is something it already knows.  Push through
 * APNs/FCM is what this becomes when the app is *closed*; until then the
 * worker is the whole of it.
 *
 * Messages are plain JSON with a `kind` discriminator, because a
 * `MessagePort` structured-clones and a class does not survive that.
 */

import type { ClientEvent } from '../events.js';
import type { Credential } from '@blacknode/irc-protocol';

/** How the worker should connect.  Sent by the first tab to ask. */
export interface WorkerConnectRequest {
  readonly url: string;
  readonly nick: string;
  readonly username?: string;
  readonly realname?: string;
  readonly password?: string;
  readonly credential?: Credential;
  readonly languages?: readonly string[];
}

/** Tab to worker. */
export type ToWorker =
  /** Connect, or say nothing if somebody already did. */
  | { readonly kind: 'connect'; readonly request: WorkerConnectRequest }
  /** Send a line exactly as given. */
  | { readonly kind: 'raw'; readonly line: string }
  /** Say something, with the splitting and the tags handled. */
  | {
      readonly kind: 'say';
      readonly target: string;
      readonly text: string;
      readonly replyTo?: string;
      readonly markdown?: boolean;
      readonly notice?: boolean;
    }
  | { readonly kind: 'join'; readonly channel: string; readonly key?: string }
  | { readonly kind: 'part'; readonly channel: string; readonly reason?: string }
  | { readonly kind: 'react'; readonly target: string; readonly messageId: string; readonly reaction: string }
  | { readonly kind: 'typing'; readonly target: string; readonly state: string }
  | { readonly kind: 'redact'; readonly target: string; readonly messageId: string; readonly reason?: string }
  | { readonly kind: 'mark-read'; readonly target: string; readonly at: string }
  | {
      readonly kind: 'history';
      readonly id: number;
      readonly target: string;
      /** A `ChatHistorySelector`, as JSON: a Date does survive a
       * structured clone, but not `JSON.stringify` through a
       * `postMessage` polyfill, so it travels as an ISO string. */
      readonly selector: unknown;
    }
  /** This tab is visible, or it is not.
   *
   * Which decides whether the worker raises a notification: a mention in
   * a tab you are looking at does not need one, and a person with eight
   * tabs should get one notification and not eight.
   */
  | { readonly kind: 'visibility'; readonly visible: boolean }
  /** Give me the state as it is now; a tab that just opened has none. */
  | { readonly kind: 'sync' }
  | { readonly kind: 'disconnect'; readonly reason?: string };

/** What a tab is told when it asks to sync.
 *
 * A snapshot rather than a replay: a tab that opens into a conversation
 * three hours old does not want three hours of events, it wants what is
 * on the screen.
 */
export interface WorkerSnapshot {
  readonly status: string;
  readonly nick: string;
  readonly identified: boolean;
  readonly frozen: boolean;
  readonly channels: readonly {
    readonly name: string;
    readonly topic?: string;
    readonly members: readonly string[];
  }[];
  readonly conversations: readonly {
    readonly target: string;
    readonly unread: number;
    readonly lastAt?: string;
  }[];
}

/** Worker to tab. */
export type FromWorker =
  /** One event, exactly as the client emitted it.
   *
   * `Date` survives a structured clone, so a `StoredMessage` arrives
   * with its `at` intact and nothing has to be revived.
   */
  | { readonly kind: 'event'; readonly event: ClientEvent }
  | { readonly kind: 'snapshot'; readonly snapshot: WorkerSnapshot }
  | { readonly kind: 'history'; readonly id: number; readonly messages: readonly unknown[] }
  /** Somebody should be told about this, and this tab is the one to do
   * it: it is the visible one, or there is no visible one.
   *
   * The worker picks *one* tab rather than broadcasting, because eight
   * tabs each raising the same notification is eight notifications.
   */
  | {
      readonly kind: 'notify';
      readonly target: string;
      readonly from: string;
      readonly text: string;
      readonly mention: boolean;
    };
