/**
 * What a client tells the code above it.
 *
 * A closed set, the way `enum HookType` is closed in the server: a union
 * a caller can exhaust is one the compiler can check, and an SDK whose
 * events are `(name: string, ...args: any[])` moves every mistake from
 * compile time to a user's phone.
 */

import type { Channel, StoredMessage, User } from './state.js';
import type { StandardReply } from '@blacknode/irc-protocol';

/** Why a connection ended. */
export type DisconnectReason =
  | 'requested'
  | 'network'
  | 'server-closed'
  | 'protocol-error'
  | 'registration-failed';

export type ClientEvent =
  /* --- the connection --- */
  | { readonly type: 'connecting'; readonly attempt: number }
  | { readonly type: 'connected' }
  /** Registration finished: this connection is a user on the network. */
  | { readonly type: 'registered'; readonly nick: string }
  | {
      readonly type: 'disconnected';
      readonly reason: DisconnectReason;
      readonly detail: string;
      /** A reconnection is already scheduled. */
      readonly willRetry: boolean;
    }

  /* --- identity --- */
  /** We are logged in, to the account named here.
   *
   * The account is not the nickname: it is the name the network's
   * services keep, and this connection may be wearing any nick.
   */
  | { readonly type: 'identified'; readonly account: string }
  | { readonly type: 'identify-failed'; readonly reason: string }
  /**
   * Somebody else logged in, or out (`ACCOUNT`, account-notify).
   *
   * `account` is undefined when they logged out.  It arrives for
   * everybody sharing a channel with them, this connection included --
   * so a UI that keys off this alone sees its own login twice, once
   * here and once as `identified`.
   */
  | { readonly type: 'account'; readonly nick: string; readonly account?: string }
  /**
   * The server renamed us.
   *
   * The services rename a client that could not show a registered
   * nickname was its own.  It arrives as an ordinary NICK from the
   * server, and it is worth showing rather than swallowing.
   */
  | { readonly type: 'renamed'; readonly from: string; readonly to: string; readonly byServer: boolean }
  /**
   * Frozen, or unfrozen (umode `+f`).
   *
   * While frozen almost nothing may be sent: the server allows only what
   * each command declares, narrowed to talking to the service that will
   * lift it.  A UI that does not show this shows a client that appears
   * to be broken.  Logging in lifts it.
   */
  | { readonly type: 'freeze'; readonly frozen: boolean }

  /* --- conversation --- */
  | { readonly type: 'message'; readonly message: StoredMessage }
  /** A reaction arrived; it is stored as a message of its own. */
  | { readonly type: 'reaction'; readonly message: StoredMessage; readonly to: string }
  /** Somebody is typing, or stopped.  Never stored. */
  | { readonly type: 'typing'; readonly target: string; readonly from: string; readonly state: string }
  /** A message was taken back.  The row stays; its text is gone. */
  | { readonly type: 'redacted'; readonly target: string; readonly id: string; readonly by: string }
  /** A message now says something else.  Only its author can do this, and
   * the identifier is the one it always had. */
  | {
      readonly type: 'edited';
      readonly target: string;
      readonly id: string;
      readonly text: string;
      readonly by: string;
    }
  /** A SEARCH answer finished arriving. */
  | {
      readonly type: 'search';
      readonly target: string;
      readonly messages: readonly StoredMessage[];
    }
  /** The read marker moved, here or on another of this person's clients. */
  | { readonly type: 'read-marker'; readonly target: string; readonly at: Date }
  /** A CHATHISTORY answer finished arriving. */
  | {
      readonly type: 'history';
      readonly target: string;
      readonly messages: readonly StoredMessage[];
    }
  /** A CHATHISTORY TARGETS answer: the conversations themselves. */
  | { readonly type: 'targets'; readonly targets: readonly { name: string; at: Date }[] }

  /* --- channels and people --- */
  | { readonly type: 'joined'; readonly channel: Channel; readonly nick: string; readonly self: boolean }
  | { readonly type: 'parted'; readonly channel: string; readonly nick: string; readonly self: boolean; readonly reason: string }
  | { readonly type: 'kicked'; readonly channel: string; readonly nick: string; readonly by: string; readonly reason: string }
  | { readonly type: 'quit'; readonly nick: string; readonly reason: string }
  | { readonly type: 'nick'; readonly from: string; readonly to: string }
  | { readonly type: 'topic'; readonly channel: string; readonly topic: string; readonly by: string }
  | { readonly type: 'mode'; readonly target: string; readonly change: string; readonly by: string }
  | { readonly type: 'names'; readonly channel: Channel }
  | { readonly type: 'user-updated'; readonly user: User }

  /* --- what the server says --- */
  /** A FAIL, WARN or NOTE.  Machine-readable, unlike a numeric's text. */
  | { readonly type: 'standard-reply'; readonly reply: StandardReply }
  | { readonly type: 'numeric'; readonly numeric: string; readonly params: readonly string[] }
  | { readonly type: 'notice'; readonly from: string; readonly target: string; readonly text: string }
  /** Every line, for a client that wants to show a raw log. */
  | { readonly type: 'raw'; readonly line: string; readonly outgoing: boolean }
  | { readonly type: 'error'; readonly error: Error };

/** A listener. */
export type ClientListener = (event: ClientEvent) => void;

/** The smallest emitter that does the job.
 *
 * Not `EventTarget` (not in React Native without a polyfill) and not
 * node's `EventEmitter` (not in a browser): both would be a dependency
 * on the runtime, which is the one thing this package does not have.
 */
export class Emitter {
  private readonly listeners = new Set<ClientListener>();

  on(fn: ClientListener): () => void {
    this.listeners.add(fn);

    return () => {
      this.listeners.delete(fn);
    };
  }

  emit(event: ClientEvent): void {
    // A copy, because a listener may remove itself -- or another -- and
    // mutating the set while iterating it would skip one.
    for (const fn of [...this.listeners]) {
      try {
        fn(event);
      } catch {
        // A listener that throws is the caller's bug, and letting it
        // out here would take the connection down with it: the next
        // listener still gets the event, and so does the next line.
      }
    }
  }

  get size(): number {
    return this.listeners.size;
  }
}
