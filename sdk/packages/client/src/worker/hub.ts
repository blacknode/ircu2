/**
 * The worker's side: one connection, many tabs.
 *
 * This file has no `SharedWorkerGlobalScope` in it and no `self`.  It is
 * given ports and it does the rest, which is what lets the whole of the
 * fan-out -- including which tab gets a notification, and what happens
 * when that tab closes mid-message -- be tested without a browser.
 * `shared-worker.ts` is the twenty lines that connect it to one.
 */

import { Client, type ClientOptions } from '../client.js';
import type { ClientEvent } from '../events.js';
import { NetworkState } from '../state.js';
import type { Transport } from '../transport.js';
import type {
  FromWorker,
  ToWorker,
  WorkerConnectRequest,
  WorkerSnapshot,
} from './protocol.js';

/** The part of `MessagePort` this uses. */
export interface PortLike {
  postMessage(value: unknown): void;
  start?(): void;
  close?(): void;
  onmessage: ((ev: { data: unknown }) => void) | null;
}

/** One tab. */
interface Tab {
  readonly port: PortLike;
  visible: boolean;
}

/** How the hub makes a transport, so a test can hand it a fake. */
export type TransportFactory = (url: string) => Transport;

export interface HubOptions {
  readonly transportFactory: TransportFactory;
  /** Everything but the transport and the identity, which a tab brings. */
  readonly clientOptions?: Partial<Omit<ClientOptions, 'transport' | 'nick'>>;
}

/** One connection, shared. */
export class WorkerHub {
  private readonly tabs = new Set<Tab>();
  private client: Client | undefined;
  private unsubscribe: (() => void) | undefined;
  private readonly historyWaiters = new Map<number, Tab>();

  constructor(private readonly options: HubOptions) {}

  /** How many tabs are attached. */
  get tabCount(): number {
    return this.tabs.size;
  }

  /** The connection, for tests and for diagnostics. */
  get connection(): Client | undefined {
    return this.client;
  }

  /** A tab connected. */
  attach(port: PortLike): void {
    const tab: Tab = { port, visible: true };

    this.tabs.add(tab);

    port.onmessage = (ev) => {
      this.onMessage(tab, ev.data as ToWorker);
    };

    port.start?.();
  }

  /** A tab went away.
   *
   * The connection stays: a person who closed one of eight tabs has not
   * gone offline, and dropping the connection would lose everything the
   * other seven are showing.  The *last* tab closing is different, and
   * even then the connection is kept -- a shared worker outlives its
   * tabs for a while, and coming back to a live connection is the point.
   */
  detach(port: PortLike): void {
    for (const tab of this.tabs) {
      if (tab.port !== port) continue;

      this.tabs.delete(tab);

      for (const [id, waiter] of this.historyWaiters) {
        if (waiter === tab) this.historyWaiters.delete(id);
      }

      return;
    }
  }

  private onMessage(tab: Tab, msg: ToWorker): void {
    switch (msg.kind) {
      case 'connect':
        this.ensureConnected(msg.request);
        this.send(tab, { kind: 'snapshot', snapshot: this.snapshot() });
        return;

      case 'sync':
        this.send(tab, { kind: 'snapshot', snapshot: this.snapshot() });
        return;

      case 'visibility':
        tab.visible = msg.visible;
        return;

      case 'disconnect':
        this.client?.disconnect(msg.reason);
        return;

      default:
        break;
    }

    const client = this.client;

    if (!client) return;

    switch (msg.kind) {
      case 'raw':
        client.sendRaw(msg.line);
        return;
      case 'say':
        client.say(msg.target, msg.text, {
          ...(msg.replyTo ? { replyTo: msg.replyTo } : {}),
          ...(msg.markdown ? { markdown: true } : {}),
          ...(msg.notice ? { notice: true } : {}),
        });
        return;
      case 'join':
        client.join(msg.channel, msg.key);
        return;
      case 'part':
        client.part(msg.channel, msg.reason);
        return;
      case 'react':
        client.react(msg.target, msg.messageId, msg.reaction);
        return;
      case 'typing':
        client.typing(msg.target, msg.state as 'active');
        return;
      case 'redact':
        client.redact(msg.target, msg.messageId, msg.reason);
        return;
      case 'mark-read':
        client.markRead(msg.target, new Date(msg.at));
        return;
      case 'history': {
        this.historyWaiters.set(msg.id, tab);

        void client
          .history(msg.target, reviveSelector(msg.selector))
          .then((messages) => {
            const waiter = this.historyWaiters.get(msg.id);

            this.historyWaiters.delete(msg.id);

            // The tab that asked may have closed while the server was
            // answering; there is nobody to give it to, and giving it to
            // somebody else would be answering a question they did not
            // ask.
            if (waiter && this.tabs.has(waiter)) {
              this.send(waiter, { kind: 'history', id: msg.id, messages });
            }
          })
          .catch(() => {
            this.historyWaiters.delete(msg.id);
          });

        return;
      }
      default:
        return;
    }
  }

  /** Connect if nobody has.  The second tab to ask is a no-op. */
  private ensureConnected(request: WorkerConnectRequest): void {
    if (this.client) return;

    const client = new Client({
      ...this.options.clientOptions,
      transport: this.options.transportFactory(request.url),
      nick: request.nick,
      ...(request.username ? { username: request.username } : {}),
      ...(request.realname ? { realname: request.realname } : {}),
      ...(request.password ? { password: request.password } : {}),
      ...(request.credential ? { credential: request.credential } : {}),
      ...(request.languages ? { languages: request.languages } : {}),
    });

    this.client = client;
    this.unsubscribe = client.on((event) => this.onEvent(event));

    client.connect();
  }

  /** Every tab sees every event; one tab is asked to notify. */
  private onEvent(event: ClientEvent): void {
    for (const tab of this.tabs) this.send(tab, { kind: 'event', event });

    if (event.type !== 'message') return;

    const msg = event.message;
    const client = this.client;

    if (!client) return;

    // Our own message, and a replayed one, are not news.
    if (msg.replayed) return;
    if (NetworkState.fold(msg.from) === NetworkState.fold(client.state.nick)) return;

    const direct = !NetworkState.isChannel(msg.target);
    const mention = direct || mentions(msg.text, client.state.nick);

    if (!mention) return;

    const chosen = this.notifyTarget();

    if (!chosen) return;

    this.send(chosen, {
      kind: 'notify',
      target: msg.target,
      from: msg.from,
      text: msg.text,
      mention: !direct,
    });
  }

  /** Which tab raises a notification.
   *
   * One of them, never all: eight tabs each raising the same
   * notification is eight notifications for one message.  A hidden tab
   * is preferred over a visible one, because a mention in a window the
   * person is looking at does not need announcing -- and if every tab is
   * visible, none does.
   */
  private notifyTarget(): Tab | undefined {
    let hidden: Tab | undefined;

    for (const tab of this.tabs) {
      if (!tab.visible) {
        hidden ??= tab;
        continue;
      }
    }

    return hidden;
  }

  private snapshot(): WorkerSnapshot {
    const client = this.client;

    if (!client) {
      return {
        status: 'idle',
        nick: '',
        identified: false,
        frozen: false,
        channels: [],
        conversations: [],
      };
    }

    const state = client.state;
    const conversations: { target: string; unread: number; lastAt?: string }[] = [];

    for (const [key, list] of state.messages) {
      const last = list[list.length - 1];

      conversations.push({
        target: last?.target ?? key,
        unread: state.unread(last?.target ?? key),
        ...(last ? { lastAt: last.at.toISOString() } : {}),
      });
    }

    return {
      status: client.connectionStatus,
      nick: state.nick,
      identified: state.identified,
      frozen: state.frozen,
      channels: [...state.channels.values()].map((c) => ({
        name: c.name,
        ...(c.topic !== undefined ? { topic: c.topic } : {}),
        members: [...c.members.values()].map((m) => m.nick),
      })),
      conversations,
    };
  }

  private send(tab: Tab, msg: FromWorker): void {
    try {
      tab.port.postMessage(msg);
    } catch {
      // A port whose tab has gone throws on post.  Dropping it here
      // rather than letting it out keeps one dead tab from stopping the
      // fan-out to the live ones.
      this.tabs.delete(tab);
    }
  }

  /** Close the connection and forget every tab.  For tests. */
  shutdown(): void {
    this.unsubscribe?.();
    this.unsubscribe = undefined;
    this.client?.disconnect();
    this.client = undefined;
    this.tabs.clear();
    this.historyWaiters.clear();
  }
}

/** Does `text` mention `nick`?
 *
 * On this server an account **is** a nickname, so mentioning `maria` is
 * mentioning the account whenever `+r` says she proved it -- there is no
 * separate thing to match against, which is why this is a word match on
 * the nickname and not a lookup.
 *
 * Word boundaries by hand rather than by regular expression: a nickname
 * may contain `[`, `]`, `\`, `^` and `{`, and several of those are
 * regular-expression syntax.
 */
export function mentions(text: string, nick: string): boolean {
  if (!nick) return false;

  const haystack = NetworkState.fold(text);
  const needle = NetworkState.fold(nick);
  let at = haystack.indexOf(needle);

  while (at >= 0) {
    const before = at === 0 ? '' : (haystack[at - 1] as string);
    const after = haystack[at + needle.length] ?? '';

    if (!isNickChar(before) && !isNickChar(after)) return true;

    at = haystack.indexOf(needle, at + 1);
  }

  return false;
}

function isNickChar(ch: string): boolean {
  if (!ch) return false;

  return /[a-z0-9_\-[\]\\^{}|`]/.test(ch);
}

/** Turn a selector that crossed a port back into one with Dates. */
function reviveSelector(value: unknown): never {
  const sel = value as Record<string, unknown>;
  const point = (p: unknown): unknown => {
    const q = p as Record<string, unknown>;

    if (q && q['kind'] === 'timestamp' && typeof q['at'] === 'string') {
      return { kind: 'timestamp', at: new Date(q['at']) };
    }

    return q;
  };

  return {
    ...sel,
    ...(sel['at'] ? { at: point(sel['at']) } : {}),
    ...(sel['from'] ? { from: point(sel['from']) } : {}),
    ...(sel['to'] ? { to: point(sel['to']) } : {}),
  } as never;
}
