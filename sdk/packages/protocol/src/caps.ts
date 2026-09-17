/**
 * CAP negotiation, as a state machine with no I/O.
 *
 * It is given the CAP lines that arrive and it says what to send next;
 * the caller does the sending.  That is what lets the whole of
 * negotiation -- including the parts that only happen on a slow or
 * unusual server -- be tested without a socket.
 *
 * The capabilities named here are the ones *this* server implements.  A
 * generic list would be worse: the point of an SDK that ships with the
 * server is that it knows what is on the other end, and a capability the
 * server never advertises is a branch nobody will ever run.
 */

import type { Message } from './message.js';

/** Capabilities this SDK knows how to use.
 *
 * Asking for one the server does not have costs nothing -- it simply is
 * not in the ACK -- so this list is what the SDK can *make use of*,
 * not what it requires.  Everything degrades: see `CapState.degraded`.
 */
export const WANTED_CAPS = [
  // The foundation.  Without message-tags there is no msgid, and without
  // msgid nothing that refers back to a message works at all.
  'message-tags',
  'server-time',
  'batch',
  'labeled-response',
  'echo-message',
  'standard-replies',

  // Identity (proposal 007).  `sasl` carries its mechanism list as the
  // capability's value, and it is withdrawn when no provider is loaded,
  // so its presence is the honest answer to "can I log in here".
  'sasl',

  // Long messages, as one message rather than several.
  'draft/multiline',
  'draft/multiline-concat',

  // Conversation (proposal 006 §7.2).
  'draft/chathistory',
  'draft/read-marker',
  'draft/message-redaction',

  // Rich text (§7.3).  The server sends plain text to anyone who did not
  // ask for this, so not having it is not a downgrade in content.
  'blacknode/richtext',

  // Per-connection language preference.
  'draft/languages',
] as const;

export type WantedCap = (typeof WANTED_CAPS)[number];

/** What happened, for the caller to act on. */
export interface CapStep {
  /** Lines to send, in order. */
  readonly send: readonly string[];
  /** Negotiation is over; the caller may send USER/NICK or CAP END. */
  readonly done: boolean;
  /** Set when the server refused something we cannot proceed without. */
  readonly error?: string;
}

const NOTHING: CapStep = { send: [], done: false };

/**
 * The state of one connection's negotiation.
 *
 * Created before the first byte goes out; `start()` produces the opening
 * `CAP LS 302`, `handle()` is fed every CAP line, and `available`/`active`
 * answer what the connection ended up with.
 */
export class CapState {
  /** Everything the server offered, name to value ("" when valueless). */
  readonly available = new Map<string, string>();

  /** Everything in force. */
  readonly active = new Set<string>();

  /** Set once CAP END has been sent, or negotiation was abandoned. */
  private ended = false;

  /** Set while CAP LS is arriving in more than one line. */
  private lsPending = false;

  /** True when the caller must wait for SASL before CAP END.
   *
   * The server sends 900/903 the moment it has taken the nickname, and a
   * client is expected to wait for that before CAP END -- but CAP END is
   * what lets registration finish, so holding the numeric would deadlock
   * both sides.  This flag is how the caller knows which of the two it
   * is waiting on.
   */
  saslPending = false;

  /** Capabilities asked for and not yet answered. */
  private requested = new Set<string>();

  /** The opening line. */
  start(): CapStep {
    // 302, because that is what makes the server send values -- and the
    // value is the whole content of `sasl` (its mechanisms) and of
    // `draft/multiline` (its limits).
    return { send: ['CAP LS 302'], done: false };
  }

  /** What the connection ended up without, of what it asked for. */
  get degraded(): string[] {
    return WANTED_CAPS.filter((c) => !this.active.has(c));
  }

  /** The `sasl` mechanisms the server offered, uppercased. */
  get saslMechanisms(): string[] {
    const value = this.available.get('sasl');

    if (value === undefined) return [];

    // An empty value means the capability is there but names no
    // mechanism, which is a server with no identity provider loaded.
    return value ? value.split(',').map((m) => m.trim().toUpperCase()).filter(Boolean) : [];
  }

  /** `draft/multiline`'s limits, or undefined when it is not in force. */
  get multilineLimits(): { maxBytes: number; maxLines: number } | undefined {
    if (!this.active.has('draft/multiline')) return undefined;

    const value = this.available.get('draft/multiline') ?? '';
    let maxBytes = 0;
    let maxLines = 0;

    for (const part of value.split(',')) {
      const [key, raw] = part.split('=');
      const n = Number(raw);

      if (!Number.isFinite(n)) continue;
      if (key === 'max-bytes') maxBytes = n;
      if (key === 'max-lines') maxLines = n;
    }

    return { maxBytes, maxLines };
  }

  /** Feed one CAP line.  Anything else returns {@link NOTHING}. */
  handle(msg: Message): CapStep {
    if (msg.command !== 'CAP') return NOTHING;

    // CAP <target> <subcommand> [*] <caps>
    const sub = (msg.params[1] ?? '').toUpperCase();

    switch (sub) {
      case 'LS':
        return this.onList(msg, true);
      case 'NEW':
        return this.onNew(msg);
      case 'DEL':
        return this.onDel(msg);
      case 'ACK':
        return this.onAck(msg, true);
      case 'NAK':
        return this.onAck(msg, false);
      default:
        return NOTHING;
    }
  }

  /** CAP LS, possibly continued. */
  private onList(msg: Message, isLs: boolean): CapStep {
    const more = msg.params[2] === '*';
    const list = (more ? msg.params[3] : msg.params[2]) ?? '';

    this.record(list);

    if (more) {
      this.lsPending = true;
      return NOTHING;
    }

    this.lsPending = false;

    if (!isLs) return NOTHING;

    const want = WANTED_CAPS.filter((c) => this.available.has(c));

    if (!want.length) {
      // A server with none of them is a perfectly ordinary IRC server and
      // this is a perfectly ordinary client on it.  End and carry on.
      return this.end();
    }

    for (const c of want) this.requested.add(c);

    // One REQ per line, split so no line runs past the limit.  A server
    // must apply a REQ atomically, so splitting changes the meaning
    // slightly -- some may be ACKed and others NAKed -- which is fine
    // here because nothing in `WANTED_CAPS` depends on another being
    // granted in the same breath.
    return { send: chunkRequests(want), done: false };
  }

  /** CAP NEW: a module was loaded while we were connected. */
  private onNew(msg: Message): CapStep {
    const list = msg.params[2] ?? '';

    this.record(list);

    const want = WANTED_CAPS.filter((c) => this.available.has(c) && !this.active.has(c));

    if (!want.length) return NOTHING;

    for (const c of want) this.requested.add(c);

    return { send: chunkRequests(want), done: false };
  }

  /** CAP DEL: a module went away.  Nothing to send; it is simply gone. */
  private onDel(msg: Message): CapStep {
    for (const name of (msg.params[2] ?? '').split(' ')) {
      if (!name) continue;

      this.available.delete(name);
      this.active.delete(name);
    }

    return NOTHING;
  }

  /** CAP ACK or CAP NAK. */
  private onAck(msg: Message, granted: boolean): CapStep {
    const names = (msg.params[2] ?? '').split(' ').filter(Boolean);

    for (const name of names) {
      // An ACK may carry a leading `-` for a capability being dropped.
      const dropping = name.startsWith('-');
      const bare = dropping ? name.slice(1) : name;

      this.requested.delete(bare);

      if (!granted) continue;

      if (dropping) this.active.delete(bare);
      else this.active.add(bare);
    }

    if (this.requested.size || this.lsPending) return NOTHING;

    // Everything asked for has been answered.  If SASL is on the table
    // the caller authenticates first and calls `finish()` itself; if not,
    // negotiation is over.
    if (this.active.has('sasl') && this.saslMechanisms.length) {
      this.saslPending = true;
      return { send: [], done: true };
    }

    return this.end();
  }

  /** Send CAP END.  Idempotent: calling it twice sends nothing twice. */
  end(): CapStep {
    if (this.ended) return { send: [], done: true };

    this.ended = true;
    this.saslPending = false;

    return { send: ['CAP END'], done: true };
  }

  /** Note the capabilities in a space-separated list, with their values. */
  private record(list: string): void {
    for (const item of list.split(' ')) {
      if (!item) continue;

      const eq = item.indexOf('=');

      if (eq < 0) this.available.set(item, '');
      else this.available.set(item.slice(0, eq), item.slice(eq + 1));
    }
  }
}

/** Break a REQ into lines that fit. */
function chunkRequests(caps: readonly string[]): string[] {
  const lines: string[] = [];
  let current: string[] = [];

  const flush = () => {
    if (current.length) lines.push(`CAP REQ :${current.join(' ')}`);
    current = [];
  };

  for (const cap of caps) {
    const would = [...current, cap].join(' ').length + 'CAP REQ :'.length;

    if (would > 400 && current.length) flush();

    current.push(cap);
  }

  flush();

  return lines;
}
