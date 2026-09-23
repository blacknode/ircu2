/**
 * A connection to the server, and everything that hangs off one.
 *
 * The transport is injected, so this file is the same code on web, on a
 * phone and on a desktop; the only runtime-specific thing in the SDK is
 * `transport.ts`, and it is four methods.
 *
 * What it is *not* is a UI framework.  It keeps state, it emits events,
 * and it has methods that send things.  Anything that renders lives
 * above it.
 */

import {
  BatchAssembler,
  CapState,
  SaslSession,
  TAG_FORMAT,
  TAG_REACT,
  TAG_REPLY,
  TAG_TYPING,
  TAG_EDITED,
  FORMAT_MARKDOWN,
  chatHistoryCommand,
  editCommand,
  searchCommand,
  formatMessage,
  joinMultiline,
  parseMessage,
  parseStandardReply,
  splitForLine,
  splitMultiline,
  type ChatHistorySelector,
  type SearchOptions,
  type Credential,
  type Message,
  type OpenBatch,
} from '@blacknode/irc-protocol';

import { Emitter, type ClientListener, type DisconnectReason } from './events.js';
import { NetworkState, type StoredMessage } from './state.js';
import type { Transport } from './transport.js';

/** How to connect and who to be. */
export interface ClientOptions {
  readonly transport: Transport;
  readonly nick: string;
  readonly username?: string;
  readonly realname?: string;
  /** A server password, which is not the same as authenticating. */
  readonly password?: string;
  /** Authenticate during registration, which is the only way to arrive
   * already wearing your own nickname. */
  readonly credential?: Credential;
  /** Preferred languages, most wanted first (IRCv3 `draft/languages`). */
  readonly languages?: readonly string[];
  /** Reconnect when the connection drops.  On by default: a phone
   * changing network is the ordinary case, not the exception. */
  readonly autoReconnect?: boolean;
  /** Longest wait between attempts, in milliseconds. */
  readonly maxBackoffMs?: number;
  /** For tests, and for a runtime whose timers are not the globals. */
  readonly setTimeout?: (fn: () => void, ms: number) => unknown;
  readonly clearTimeout?: (handle: unknown) => void;
}

/** Where a connection is in its life. */
export type ClientStatus = 'idle' | 'connecting' | 'registering' | 'ready' | 'closed';

/** What to send with a message. */
export interface SendOptions {
  /** This message follows that one. */
  readonly replyTo?: string;
  /** The body is Markdown.  Sent only when `blacknode/richtext` is in
   * force; every other client is sent the plain text the *server*
   * generated from it, which is the whole design and not a nicety. */
  readonly markdown?: boolean;
  /** A NOTICE rather than a PRIVMSG. */
  readonly notice?: boolean;
}

export class Client {
  readonly state = new NetworkState();
  readonly caps = new CapState();

  private readonly emitter = new Emitter();
  private readonly batches = new BatchAssembler();
  private readonly transport: Transport;

  private status: ClientStatus = 'idle';
  private sasl: SaslSession | undefined;
  private attempt = 0;
  private retryHandle: unknown;
  private closedByUs = false;

  /** Labels in flight, and what to do with the answer. */
  private readonly labels = new Map<string, (batch: OpenBatch | undefined) => void>();
  private nextLabel = 1;

  constructor(private readonly options: ClientOptions) {
    this.transport = options.transport;
  }

  /* ----------------------------------------------------------------- *
   * Lifecycle                                                          *
   * ----------------------------------------------------------------- */

  on(listener: ClientListener): () => void {
    return this.emitter.on(listener);
  }

  get connectionStatus(): ClientStatus {
    return this.status;
  }

  /** Open the connection. */
  connect(): void {
    if (this.status === 'connecting' || this.status === 'registering' || this.status === 'ready') {
      return;
    }

    this.closedByUs = false;
    this.status = 'connecting';
    this.attempt++;
    this.emitter.emit({ type: 'connecting', attempt: this.attempt });

    this.transport.connect({
      onOpen: () => this.onOpen(),
      onLine: (line) => this.onLine(line),
      onClose: (clean, reason) => this.onClose(clean, reason),
      onError: (error) => this.emitter.emit({ type: 'error', error }),
    });
  }

  /** Close it, and do not come back. */
  disconnect(reason = 'Leaving'): void {
    this.closedByUs = true;
    this.cancelRetry();

    if (this.transport.connected) this.sendRaw(formatMessage({ command: 'QUIT', params: [reason] }));

    this.transport.close(1000, reason);
  }

  private onOpen(): void {
    this.status = 'registering';
    this.batches.reset();
    this.emitter.emit({ type: 'connected' });

    // CAP first, always.  Registration is what CAP END releases, so a
    // client that sent USER/NICK before negotiating would have finished
    // registering before it knew what the server could do.
    for (const line of this.caps.start().send) this.sendRaw(line);

    if (this.options.password) {
      this.sendRaw(formatMessage({ command: 'PASS', params: [this.options.password] }));
    }

    const user = this.options.username ?? this.options.nick;

    this.sendRaw(formatMessage({ command: 'NICK', params: [this.options.nick] }));
    this.sendRaw(
      formatMessage({
        command: 'USER',
        params: [user, '0', '*', this.options.realname ?? this.options.nick],
      }),
    );

    if (this.options.languages?.length) {
      this.sendRaw(formatMessage({ command: 'LANGUAGE', params: [...this.options.languages] }));
    }
  }

  private onClose(clean: boolean, detail: string): void {
    const wasReady = this.status === 'ready';

    this.status = 'closed';
    this.sasl = undefined;
    this.batches.reset();
    this.labels.clear();
    this.state.reset();

    let reason: DisconnectReason;

    if (this.closedByUs) reason = 'requested';
    else if (clean) reason = 'server-closed';
    else if (wasReady) reason = 'network';
    else reason = 'registration-failed';

    const willRetry = !this.closedByUs && (this.options.autoReconnect ?? true);

    this.emitter.emit({ type: 'disconnected', reason, detail, willRetry });

    if (willRetry) this.scheduleRetry();
  }

  /** Back off, with a cap and with jitter.
   *
   * The jitter is not decoration: when a server restarts, every client
   * it had reconnects at once, and a fixed schedule turns that into a
   * thundering herd that keeps the server from coming up at all.
   */
  private scheduleRetry(): void {
    const max = this.options.maxBackoffMs ?? 60_000;
    const base = Math.min(max, 1000 * 2 ** Math.min(this.attempt, 6));
    const wait = Math.round(base / 2 + Math.random() * (base / 2));
    const set = this.options.setTimeout ?? ((fn, ms) => setTimeout(fn, ms));

    this.retryHandle = set(() => {
      this.retryHandle = undefined;
      this.connect();
    }, wait);
  }

  private cancelRetry(): void {
    if (this.retryHandle === undefined) return;

    const clear = this.options.clearTimeout ?? ((h) => clearTimeout(h as never));

    clear(this.retryHandle);
    this.retryHandle = undefined;
  }

  /* ----------------------------------------------------------------- *
   * Sending                                                            *
   * ----------------------------------------------------------------- */

  /** Send a line exactly as given.  The escape hatch, and a raw log. */
  sendRaw(line: string): void {
    this.emitter.emit({ type: 'raw', line, outgoing: true });
    this.transport.send(line);
  }

  /**
   * Say something.
   *
   * Handles the three things a caller should not have to: a message too
   * long for one line (as a `draft/multiline` batch where the server
   * takes one, and as separate messages where it does not), the reply
   * tag, and rich text.
   */
  say(target: string, text: string, options: SendOptions = {}): void {
    const command = options.notice ? 'NOTICE' : 'PRIVMSG';
    const tags: Record<string, string | true> = {};

    if (options.replyTo) tags[TAG_REPLY] = options.replyTo;

    // Rich text is *asked for*: the server sends the plain-text
    // equivalent to every client that did not negotiate it, so marking a
    // body as Markdown on a server that never agreed would be marking it
    // for nobody.
    if (options.markdown && this.caps.active.has('blacknode/richtext')) {
      tags[TAG_FORMAT] = FORMAT_MARKDOWN;
    }

    // What the line costs besides the text: the server's own prefix is
    // not counted (the server adds it), but the command, the target and
    // the framing are.
    const overhead = formatMessage({ command, params: [target, ''] }).length + 2;
    const limits = this.caps.multilineLimits;

    if (limits && (text.includes('\n') || overhead + text.length > 512)) {
      const pieces = splitMultiline(text, overhead, limits);

      if (pieces.length) {
        this.sendMultiline(command, target, pieces, tags);
        return;
      }
      // Too big even for a batch: fall through and send it as separate
      // messages, which is what a client without the capability does.
    }

    // Newlines with no multiline capability are separate messages: there
    // is no way to put one on the wire.
    for (const line of text.split('\n')) {
      for (const piece of splitForLine(line, overhead)) {
        this.sendRaw(formatMessage({ tags, command, params: [target, piece] }));
      }
    }
  }

  /** Send one message as a client batch. */
  private sendMultiline(
    command: string,
    target: string,
    pieces: readonly { text: string; concat: boolean }[],
    tags: Record<string, string | true>,
  ): void {
    const id = `ml${this.nextLabel++}`;

    this.sendRaw(
      formatMessage({ tags, command: 'BATCH', params: [`+${id}`, 'draft/multiline', target] }),
    );

    for (const piece of pieces) {
      const pieceTags: Record<string, string | true> = { batch: id };

      if (piece.concat) pieceTags['draft/multiline-concat'] = true;

      this.sendRaw(formatMessage({ tags: pieceTags, command, params: [target, piece.text] }));
    }

    this.sendRaw(formatMessage({ command: 'BATCH', params: [`-${id}`] }));
  }

  /** `/me`. */
  action(target: string, text: string): void {
    this.say(target, `ACTION ${text}`);
  }

  /** React to a message.
   *
   * A reaction is a TAGMSG carrying both tags: the reply tag says what
   * it is about, the react tag says it is a reaction.  The server stores
   * it as a message of its own, which is what makes removing one and
   * reading them back in order ordinary operations.
   */
  react(target: string, messageId: string, reaction: string): void {
    this.sendRaw(
      formatMessage({
        tags: { [TAG_REPLY]: messageId, [TAG_REACT]: reaction },
        command: 'TAGMSG',
        params: [target],
      }),
    );
  }

  /** Say whether we are typing.  Never stored by the server. */
  typing(target: string, state: 'active' | 'paused' | 'done' = 'active'): void {
    this.sendRaw(
      formatMessage({ tags: { [TAG_TYPING]: state }, command: 'TAGMSG', params: [target] }),
    );
  }

  join(channel: string, key?: string): void {
    this.sendRaw(
      formatMessage({ command: 'JOIN', params: key ? [channel, key] : [channel] }),
    );
  }

  part(channel: string, reason?: string): void {
    this.sendRaw(
      formatMessage({ command: 'PART', params: reason ? [channel, reason] : [channel] }),
    );
  }

  /** Take a message back.
   *
   * Who may: its author within the server's window, a channel operator
   * with no window, or an operator holding `history_admin`.  Nothing is
   * relayed until the deletion has actually happened, so a refusal comes
   * back as a standard reply rather than as a message vanishing here and
   * staying in the store.
   */
  redact(target: string, messageId: string, reason?: string): void {
    this.sendRaw(
      formatMessage({
        command: 'REDACT',
        params: reason ? [target, messageId, reason] : [target, messageId],
      }),
    );
  }

  /**
   * Move the read marker.
   *
   * Per *account*, not per connection: a person reads on their phone and
   * expects their laptop to know.  It only ever moves forward -- two
   * clients of the same person race constantly, and a marker that could
   * go back would make messages unread again every time the slower one
   * reported in.  Asking it to go back is not an error; the answer is
   * where it already was.
   */
  markRead(target: string, at: Date): void {
    this.sendRaw(
      formatMessage({
        command: 'MARKREAD',
        params: [target, `timestamp=${at.toISOString()}`],
      }),
    );
  }

  /** Ask where the marker is, without moving it. */
  readMarker(target: string): void {
    this.sendRaw(formatMessage({ command: 'MARKREAD', params: [target] }));
  }

  /**
   * Ask for history.
   *
   * The answer arrives as a batch and is emitted as one `history` event
   * rather than a burst of `message` events, because a hundred messages
   * scrolling into view is one thing happening and not a hundred.
   */
  async history(target: string, selector: ChatHistorySelector): Promise<readonly StoredMessage[]> {
    const line = chatHistoryCommand(target, selector);
    const batch = await this.withLabel(line);

    if (!batch) return [];

    const out: StoredMessage[] = [];

    for (const msg of batch.messages) {
      const stored = this.messageFrom(msg, true);

      if (!stored) continue;

      this.state.store(stored.target, stored);
      out.push(stored);
    }

    this.emitter.emit({ type: 'history', target, messages: out });

    return out;
  }

  /**
   * Change what one of your own messages says.
   *
   * Only its author, and only inside the server's window: a channel
   * operator may {@link redact} a message, because taking something out
   * of a room is not the same as making it say something its author did
   * not.  The identifier does not change -- the replies and the reactions
   * point at it -- and nothing is relayed until the store has been
   * changed, so a refusal comes back as a standard reply rather than as
   * text changing here and staying as it was everywhere else.
   */
  edit(target: string, messageId: string, text: string): void {
    this.sendRaw(editCommand(target, messageId, text));
  }

  /**
   * Search what was said.
   *
   * The answer is a batch of the messages themselves, so it arrives the
   * way history does and each one carries the target it was sent to --
   * a search of `*` answers with messages from several places at once.
   * What may be searched is what this connection may read, asked at the
   * moment it asks: the channels it is on and its own conversations.
   */
  async search(
    target: string,
    text: string,
    opts: SearchOptions = {},
  ): Promise<readonly StoredMessage[]> {
    const batch = await this.withLabel(searchCommand(target, text, opts));

    if (!batch) return [];

    const out: StoredMessage[] = [];

    for (const msg of batch.messages) {
      const stored = this.messageFrom(msg, true);

      if (!stored) continue;

      // Stored like any other replayed message: a search result is a
      // message this client now has, not a separate kind of thing.
      this.state.store(stored.target, stored);
      out.push(stored);
    }

    this.emitter.emit({ type: 'search', target, messages: out });

    return out;
  }

  /**
   * Send a line and wait for the answer it produces.
   *
   * IRCv3 `labeled-response`: the client names its request and the
   * server names the answer, so two requests in flight cannot be
   * confused for one another.  Without the capability there is nothing
   * to correlate on, so this resolves with what the batch mechanism
   * gives it and a caller gets the same shape either way.
   */
  private withLabel(line: string): Promise<OpenBatch | undefined> {
    if (!this.caps.active.has('labeled-response') || !this.caps.active.has('batch')) {
      this.sendRaw(line);
      return Promise.resolve(undefined);
    }

    const label = `l${this.nextLabel++}`;
    const parsed = parseMessage(line);

    if (!parsed) return Promise.resolve(undefined);

    return new Promise((resolve) => {
      this.labels.set(label, resolve);

      this.sendRaw(
        formatMessage({
          tags: { ...(parsed.tags as Record<string, string>), label },
          command: parsed.command,
          params: parsed.params,
        }),
      );
    });
  }

  /* ----------------------------------------------------------------- *
   * Receiving                                                          *
   * ----------------------------------------------------------------- */

  private onLine(line: string): void {
    this.emitter.emit({ type: 'raw', line, outgoing: false });

    const msg = parseMessage(line);

    if (!msg) return;

    // PING before anything else: an unanswered one is a dropped
    // connection, whatever else is going on.
    if (msg.command === 'PING') {
      this.sendRaw(formatMessage({ command: 'PONG', params: msg.params.slice() }));
      return;
    }

    // CAP and SASL run outside the batch machinery: they happen before
    // registration, when there are no batches.
    if (msg.command === 'CAP') {
      this.handleCap(msg);
      return;
    }

    if (this.sasl && (msg.command === 'AUTHENTICATE' || isSaslNumeric(msg.command))) {
      this.handleSasl(msg);
      return;
    }

    const event = this.batches.handle(msg);

    // A batch the client has a meaning for -- a labeled answer, a
    // multiline message -- is consumed whole.  Anything else falls
    // through and its lines are dispatched one at a time, which is what
    // a plain batch is: several things that happened together.
    if (event.closed && this.onBatchClosed(event.closed)) return;

    for (const m of event.deliver) this.dispatch(m);
  }

  /** @returns Whether the batch was consumed. */
  private onBatchClosed(batch: OpenBatch): boolean {
    // A labeled response: hand the whole thing to whoever asked.  The
    // label rides on the line that *opened* the batch -- never on the
    // messages inside it, nor on the closing line -- which is why the
    // assembler keeps that line's tags.
    const label = batch.tags['label'];

    if (label) {
      const waiting = this.labels.get(label);

      if (waiting) {
        this.labels.delete(label);
        waiting(batch);
        return true;
      }
    }

    // A multiline message: one message, however many lines carried it.
    if (batch.type === 'draft/multiline') {
      const text = joinMultiline(batch);
      const first = batch.messages[0];

      if (text !== undefined && first) {
        // The whole message carries *one* msgid, and it is on the line
        // that opened the batch: the pieces carry none.  Anything that
        // refers back to this message -- a reply, a reaction, a
        // redaction -- refers to that one.
        const stored = this.messageFrom(
          {
            ...first,
            tags: { ...first.tags, ...(batch.tags['msgid'] ? { msgid: batch.tags['msgid'] } : {}) },
            params: [first.params[0] ?? '', text],
          },
          false,
        );

        if (stored) {
          this.state.store(stored.target, stored);
          this.emitter.emit({ type: 'message', message: stored });
        }
      }
      return true;
    }

    return false;
  }

  private handleCap(msg: Message): void {
    const step = this.caps.handle(msg);

    for (const line of step.send) this.sendRaw(line);

    if (!this.caps.saslPending) return;

    const cred = this.options.credential;

    if (!cred) {
      for (const line of this.caps.end().send) this.sendRaw(line);
      return;
    }

    const offered = this.caps.saslMechanisms;

    if (!offered.includes(cred.mechanism)) {
      this.emitter.emit({
        type: 'identify-failed',
        reason: `the server does not offer ${cred.mechanism}`,
      });
      for (const line of this.caps.end().send) this.sendRaw(line);
      return;
    }

    this.sasl = new SaslSession(cred);

    for (const line of this.sasl.start().send) this.sendRaw(line);
  }

  private handleSasl(msg: Message): void {
    if (!this.sasl) return;

    const step = this.sasl.handle(msg.command, msg.params);

    for (const line of step.send) this.sendRaw(line);

    if (!step.outcome) return;

    this.sasl = undefined;

    if (step.outcome.ok) {
      this.state.identified = true;
      this.emitter.emit({ type: 'identified', account: step.outcome.account });
    } else {
      this.emitter.emit({ type: 'identify-failed', reason: step.outcome.reason });
    }

    // Either way, registration has been waiting on this.
    for (const line of this.caps.end().send) this.sendRaw(line);
  }

  /** Everything that is not CAP, SASL or a batch. */
  private dispatch(msg: Message): void {
    const std = parseStandardReply(msg.command, msg.params);

    if (std) {
      this.emitter.emit({ type: 'standard-reply', reply: std });
      return;
    }

    switch (msg.command) {
      case '001':
        this.onWelcome(msg);
        return;
      case '005':
        this.onIsupport(msg);
        return;
      case '332':
        this.onTopic(msg);
        return;
      case '353':
        this.onNames(msg);
        return;
      case '366':
        this.onNamesEnd(msg);
        return;
      case 'PRIVMSG':
      case 'NOTICE':
        this.onMessage(msg);
        return;
      case 'TAGMSG':
        this.onTagmsg(msg);
        return;
      case 'JOIN':
        this.onJoin(msg);
        return;
      case 'ACCOUNT':
        this.onAccount(msg);
        return;
      case 'PART':
        this.onPart(msg);
        return;
      case 'KICK':
        this.onKick(msg);
        return;
      case 'QUIT':
        this.onQuit(msg);
        return;
      case 'NICK':
        this.onNick(msg);
        return;
      case 'TOPIC':
        this.onTopicChange(msg);
        return;
      case 'MODE':
        this.onMode(msg);
        return;
      case 'EDIT':
        this.onEdit(msg);
        break;

      case 'REDACT':
        this.onRedact(msg);
        return;
      case 'MARKREAD':
        this.onMarkRead(msg);
        return;
      case 'ERROR':
        this.emitter.emit({ type: 'error', error: new Error(msg.params[0] ?? 'server error') });
        return;
      default:
        break;
    }

    if (/^\d{3}$/.test(msg.command)) {
      this.emitter.emit({ type: 'numeric', numeric: msg.command, params: msg.params });
    }
  }

  private onWelcome(msg: Message): void {
    this.status = 'ready';
    this.attempt = 0;
    this.state.nick = msg.params[0] ?? this.options.nick;
    this.state.serverName = msg.prefix?.raw ?? '';
    this.emitter.emit({ type: 'registered', nick: this.state.nick });
  }

  private onIsupport(msg: Message): void {
    // 005 is "<nick> <token>... :are supported by this server", so the
    // first and last parameters are not tokens.
    for (const token of msg.params.slice(1, -1)) {
      const eq = token.indexOf('=');

      if (eq < 0) this.state.isupport.set(token, '');
      else this.state.isupport.set(token.slice(0, eq), token.slice(eq + 1));
    }
  }

  private onTopic(msg: Message): void {
    const channel = this.state.touchChannel(msg.params[1] ?? '');

    channel.topic = msg.params[2] ?? '';
  }

  private onNames(msg: Message): void {
    const channel = this.state.touchChannel(msg.params[2] ?? '');

    for (const entry of (msg.params[3] ?? '').split(' ')) {
      if (!entry) continue;

      let nick = entry;
      let op = false;
      let voice = false;

      while (nick.startsWith('@') || nick.startsWith('+')) {
        if (nick[0] === '@') op = true;
        if (nick[0] === '+') voice = true;
        nick = nick.slice(1);
      }

      this.state.touchUser(nick);
      channel.members.set(NetworkState.fold(nick), { nick, op, voice });
    }
  }

  private onNamesEnd(msg: Message): void {
    const channel = this.state.touchChannel(msg.params[1] ?? '');

    channel.joining = false;
    this.emitter.emit({ type: 'names', channel });
  }

  private onJoin(msg: Message): void {
    const nick = msg.prefix?.nick ?? '';
    const name = msg.params[0] ?? '';
    const self = NetworkState.fold(nick) === NetworkState.fold(this.state.nick);
    const channel = this.state.touchChannel(name);

    const user = this.state.touchUser(nick);

    if (msg.prefix?.user) user.user = msg.prefix.user;
    if (msg.prefix?.host) user.host = msg.prefix.host;

    // extended-join: JOIN <channel> <account> :<real name>, with "*" for
    // somebody who is not logged in.  Without the capability the server
    // sends the plain form and the two extra parameters are simply not
    // there, which is why nothing below is conditional on the cap.
    if (msg.params.length >= 3) {
      const account = msg.params[1] ?? '*';
      const realname = msg.params[2];

      user.identified = account !== '*';
      if (account === '*') delete user.account;
      else user.account = account;
      if (realname !== undefined) user.realname = realname;
    }

    channel.members.set(NetworkState.fold(nick), { nick, op: false, voice: false });

    this.emitter.emit({ type: 'joined', channel, nick, self });
  }

  /** `ACCOUNT` (account-notify): somebody logged in, or out.
   *
   * The parameter is the account name, or `*` for a logout.  The
   * server sends the name alone -- the id and the flags that ride with
   * it between servers never reach a client.
   */
  private onAccount(msg: Message): void {
    const nick = msg.prefix?.nick ?? '';
    const param = msg.params[0] ?? '*';
    const account = param === '*' ? undefined : param;
    const user = this.state.touchUser(nick);

    user.identified = account !== undefined;
    if (account === undefined) delete user.account;
    else user.account = account;

    if (NetworkState.fold(nick) === NetworkState.fold(this.state.nick)) {
      this.state.identified = user.identified;
      this.state.account = account;
    }

    this.emitter.emit({ type: 'account', nick, ...(account !== undefined ? { account } : {}) });
  }

  private onPart(msg: Message): void {
    const nick = msg.prefix?.nick ?? '';
    const name = msg.params[0] ?? '';
    const self = NetworkState.fold(nick) === NetworkState.fold(this.state.nick);

    if (self) this.state.channels.delete(NetworkState.fold(name));
    else this.state.channel(name)?.members.delete(NetworkState.fold(nick));

    this.emitter.emit({ type: 'parted', channel: name, nick, self, reason: msg.params[1] ?? '' });
  }

  private onKick(msg: Message): void {
    const name = msg.params[0] ?? '';
    const nick = msg.params[1] ?? '';

    if (NetworkState.fold(nick) === NetworkState.fold(this.state.nick)) {
      this.state.channels.delete(NetworkState.fold(name));
    } else {
      this.state.channel(name)?.members.delete(NetworkState.fold(nick));
    }

    this.emitter.emit({
      type: 'kicked',
      channel: name,
      nick,
      by: msg.prefix?.nick ?? '',
      reason: msg.params[2] ?? '',
    });
  }

  private onQuit(msg: Message): void {
    const nick = msg.prefix?.nick ?? '';

    this.state.removeUser(nick);
    this.emitter.emit({ type: 'quit', nick, reason: msg.params[0] ?? '' });
  }

  private onNick(msg: Message): void {
    const from = msg.prefix?.nick ?? '';
    const to = msg.params[0] ?? '';
    const self = NetworkState.fold(from) === NetworkState.fold(this.state.nick);

    this.state.renameUser(from, to);

    // A rename that came from the server rather than from a person is
    // the `guest-*` rename: either we logged out, or a grace period ran
    // out on a registered nickname we never proved.  Both are worth
    // showing rather than swallowing, and the prefix is how they are
    // told apart -- a server renaming somebody uses the *old nickname*
    // as the prefix, so what says "the server did this" is that we did
    // not ask.
    if (self) {
      this.emitter.emit({
        type: 'renamed',
        from,
        to,
        byServer: !this.expectedNickChange,
      });
      this.expectedNickChange = false;
    }

    this.emitter.emit({ type: 'nick', from, to });
  }

  /** Set by `changeNick()`, cleared by the NICK that comes back. */
  private expectedNickChange = false;

  /** Ask for a different nickname.
   *
   * Which says nothing about the account: they are two names for two
   * things, and a nick change leaves `+r` where it was.
   */
  changeNick(nick: string): void {
    this.expectedNickChange = true;
    this.sendRaw(formatMessage({ command: 'NICK', params: [nick] }));
  }

  private onTopicChange(msg: Message): void {
    const name = msg.params[0] ?? '';
    const topic = msg.params[1] ?? '';
    const channel = this.state.touchChannel(name);

    channel.topic = topic;
    if (msg.prefix?.nick) channel.topicSetBy = msg.prefix.nick;
    channel.topicSetAt = new Date();

    this.emitter.emit({ type: 'topic', channel: name, topic, by: msg.prefix?.nick ?? '' });
  }

  private onMode(msg: Message): void {
    const target = msg.params[0] ?? '';
    const change = msg.params.slice(1).join(' ');

    if (NetworkState.fold(target) === NetworkState.fold(this.state.nick)) {
      this.applyUserModes(msg.params[1] ?? '', msg.params.slice(2));
    }

    this.emitter.emit({ type: 'mode', target, change, by: msg.prefix?.nick ?? '' });
  }

  /** Track our own `+r` and `+f`, which change what we may do.
   *
   * `+r` takes the account name as its parameter, so the letters are
   * read alongside whatever followed them; `args` is consumed in order,
   * the way the server writes it.  There is no `-r`: the server cannot
   * take away what it did not give, so a mode string asking for one
   * changes nothing here either.
   */
  private applyUserModes(spec: string, args: readonly string[] = []): void {
    let adding = true;
    let next = 0;

    for (const ch of spec) {
      if (ch === '+') {
        adding = true;
        continue;
      }
      if (ch === '-') {
        adding = false;
        continue;
      }

      if (adding) this.state.modes.add(ch);
      else this.state.modes.delete(ch);

      if (ch === 'r' && adding) {
        const account = args[next++] ?? this.state.account;

        this.state.identified = true;
        // "<account>:<id>:<flags>" is one parameter on the wire; the id
        // and the flags are between servers and mean nothing here.
        this.state.account = account?.split(':')[0];

        if (this.state.account) {
          this.emitter.emit({ type: 'identified', account: this.state.account });
        }
      }

      if (ch === 'f' && this.state.frozen !== adding) {
        this.state.frozen = adding;
        this.emitter.emit({ type: 'freeze', frozen: adding });
      }
    }
  }

  private onMessage(msg: Message): void {
    const stored = this.messageFrom(msg, false);

    if (!stored) return;

    this.state.store(stored.target, stored);

    if (msg.command === 'NOTICE' && !stored.action) {
      this.emitter.emit({
        type: 'notice',
        from: stored.from,
        target: stored.target,
        text: stored.text,
      });
    }

    this.emitter.emit({ type: 'message', message: stored });
  }

  private onTagmsg(msg: Message): void {
    const from = msg.prefix?.nick ?? '';
    const target = msg.params[0] ?? '';
    const typing = msg.tags[TAG_TYPING];

    if (typing !== undefined) {
      this.emitter.emit({ type: 'typing', target, from, state: typing || 'active' });
      return;
    }

    const react = msg.tags[TAG_REACT];
    const to = msg.tags[TAG_REPLY];

    if (react !== undefined && to) {
      const stored = this.messageFrom(msg, false);

      if (stored) {
        this.state.store(stored.target, stored);
        this.emitter.emit({ type: 'reaction', message: stored, to });
      }
    }
  }

  private onRedact(msg: Message): void {
    const target = msg.params[0] ?? '';
    const id = msg.params[1] ?? '';

    this.state.redact(target, id);
    this.emitter.emit({ type: 'redacted', target, id, by: msg.prefix?.nick ?? '' });
  }

  private onEdit(msg: Message): void {
    const target = msg.params[0] ?? '';
    const id = msg.params[1] ?? '';
    const text = msg.params[2] ?? '';

    if (!target || !id) return;

    // The conversation this belongs to, not the target as written: a
    // direct message is filed under the other end either way round.
    const mine = NetworkState.fold(target) === NetworkState.fold(this.state.nick);
    const by = msg.prefix?.nick ?? '';
    const where = NetworkState.isChannel(target) ? target : mine ? by : target;

    this.state.edit(where, id, text, new Date());
    this.emitter.emit({ type: 'edited', target: where, id, text, by });
  }

  private onMarkRead(msg: Message): void {
    const target = msg.params[0] ?? '';
    const spec = msg.params[1] ?? '';
    const at = spec.startsWith('timestamp=') ? new Date(spec.slice('timestamp='.length)) : undefined;

    if (!at || Number.isNaN(at.getTime())) return;

    this.state.readMarkers.set(NetworkState.fold(target), at);
    this.emitter.emit({ type: 'read-marker', target, at });
  }

  /** Turn a PRIVMSG, NOTICE or TAGMSG into something to keep. */
  private messageFrom(msg: Message, replayed: boolean): StoredMessage | undefined {
    const from = msg.prefix?.nick ?? '';
    const target = msg.params[0] ?? '';

    if (!target) return undefined;

    let text = msg.params[1] ?? '';
    let action = false;

    // CTCP ACTION is `/me`; every other CTCP is a client-to-client
    // protocol this SDK has no opinion about and passes through as text.
    if (text.startsWith('ACTION ') && text.endsWith('')) {
      text = text.slice('ACTION '.length, -1);
      action = true;
    }

    // `server-time` is why a replayed message shows when it was *sent*
    // and not when it was replayed.
    const timeTag = msg.tags['time'];
    const at = timeTag ? new Date(timeTag) : new Date();

    // A direct message is filed under the other end, not under us: what
    // a person thinks of as "the conversation with maria" is one thing
    // whichever way a given message went.
    const mine = NetworkState.fold(target) === NetworkState.fold(this.state.nick);
    const conversation = NetworkState.isChannel(target) ? target : mine ? from : target;

    const react = msg.tags[TAG_REACT];
    const replyTo = msg.tags[TAG_REPLY];
    const format = msg.tags[TAG_FORMAT];

    // Only a replayed message carries this: live, a change arrives as an
    // EDIT.  It is the server's own tag, so nothing a client sent can put
    // it here.
    const editedTag = msg.tags[TAG_EDITED];
    const edited = editedTag ? new Date(editedTag) : undefined;

    return {
      id: msg.tags['msgid'] ?? undefined,
      target: conversation,
      from,
      text,
      at: Number.isNaN(at.getTime()) ? new Date() : at,
      notice: msg.command === 'NOTICE',
      action,
      ...(replyTo ? { replyTo } : {}),
      ...(react !== undefined ? { react } : {}),
      ...(format === FORMAT_MARKDOWN ? { markdown: true } : {}),
      ...(replayed ? { replayed: true } : {}),
      ...(edited && !Number.isNaN(edited.getTime()) ? { edited } : {}),
    };
  }
}

function isSaslNumeric(command: string): boolean {
  return ['900', '901', '902', '903', '904', '905', '906', '907', '908'].includes(command);
}
