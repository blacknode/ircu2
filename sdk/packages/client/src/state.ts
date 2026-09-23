/**
 * What the client knows about the network it is on.
 *
 * Plain data with no I/O: `Client` feeds it lines and the UI reads it.
 *
 * The one thing worth stating twice: **an account is not a nickname.**
 * The network's services keep the accounts, and they say which one a
 * user is logged in to; the nickname is what that user happens to be
 * wearing.  So `account` and `nick` are two fields, a nick change
 * leaves both alone, and a UI that wants to say "this really is maria"
 * has to show the account rather than the prefix.
 */

/** Somebody on the network. */
export interface User {
  nick: string;
  user?: string;
  host?: string;
  realname?: string;
  /** They are logged in to an account (umode `+r`).
   *
   * When it is false, the nickname is just a nickname somebody is
   * wearing and an onlooker cannot tell it from an impostor.
   */
  identified: boolean;
  /** The account they are logged in to, when {@link identified}.
   *
   * At most twelve characters, and not the nickname: a person may wear
   * any nick while logged in to `maria`.  It arrives on an `ACCOUNT`
   * (account-notify), on a `JOIN` (extended-join), or from `WHOIS` 330
   * and `WHO %a` when neither capability is in force.
   */
  account?: string;
  /** Held by the services while they work out whether this person may
   * keep the nickname they are wearing (umode `+f`).
   *
   * A frozen client may send almost nothing -- the server allows only
   * what each command declares with `MFLG_FROZEN_OK`, narrowed to
   * talking to the service that will lift it -- so a UI that does not
   * show this shows a client that appears to be broken.
   */
  frozen: boolean;
  away?: string;
  /** Their connection is TLS. */
  secure?: boolean;
  /** A bot of the network (umode `+B`) or a service (`+S`). */
  bot?: boolean;
  service?: boolean;
}

/** Somebody's standing in one channel. */
export interface Membership {
  readonly nick: string;
  op: boolean;
  voice: boolean;
}

/** A channel this connection is in. */
export interface Channel {
  readonly name: string;
  topic?: string;
  topicSetBy?: string;
  topicSetAt?: Date;
  /** Mode letters in force, without their parameters. */
  modes: Set<string>;
  key?: string;
  limit?: number;
  readonly members: Map<string, Membership>;
  /** True until the end-of-names numeric, so a UI can wait. */
  joining: boolean;
}

/** One message, as the client keeps it. */
export interface StoredMessage {
  /** The network-wide name for it.  Everything that refers back to a
   * message -- a reply, a reaction, a redaction -- refers to this. */
  readonly id: string | undefined;
  readonly target: string;
  readonly from: string;
  /** Not readonly: an edit changes what a message says without changing
   * which message it is -- the identifier stays, because the replies and
   * the reactions point at it. */
  text: string;
  readonly at: Date;
  readonly notice: boolean;
  /** An action (`/me`), which arrives as CTCP ACTION. */
  readonly action: boolean;
  /** This is a reply to that message. */
  readonly replyTo?: string;
  /** This is a reaction, and this is the reaction. */
  readonly react?: string;
  /** The body was Markdown and this client asked for it.
   *
   * A client without `blacknode/richtext` is sent the plain text the
   * *server* generated, so this being false does not mean content was
   * lost -- it means the server did the rendering.
   */
  readonly markdown?: boolean;
  /** It came out of CHATHISTORY rather than off the wire just now. */
  readonly replayed?: boolean;
  /** Redacted: the text is gone and this is what is left.
   *
   * Kept rather than deleted so that a reply pointing at it still has
   * something to point at -- the server leaves replies alone when it
   * redacts, because a reply is somebody else's message.
   */
  redacted?: boolean;
  /** When the author last changed it, if they did.
   *
   * Live, that arrives as an `EDIT`; on a message replayed out of the
   * store it is the `blacknode/edited` server tag, because a client being
   * handed the text a second time has no other way to know it is not the
   * text that was sent.
   */
  edited?: Date;
}

/** Everything one connection knows. */
export class NetworkState {
  /** This connection's own nickname. */
  nick = '';

  /** We are logged in to an account (umode `+r`). */
  identified = false;

  /** The account we are logged in to, when {@link identified}. */
  account: string | undefined;

  /** We are frozen (umode `+f`). */
  frozen = false;

  /** Our own user modes, as letters. */
  readonly modes = new Set<string>();

  /** The server's name, from the welcome. */
  serverName = '';

  /** What `RPL_ISUPPORT` said, raw. */
  readonly isupport = new Map<string, string>();

  readonly channels = new Map<string, Channel>();
  readonly users = new Map<string, User>();

  /** Messages, newest last, per target.  Bounded; see {@link keep}. */
  readonly messages = new Map<string, StoredMessage[]>();

  /** Read markers, per target: everything up to here has been seen. */
  readonly readMarkers = new Map<string, Date>();

  /** How many messages to keep per target.
   *
   * The store is the *server's*; this is a window onto it, and a client
   * that kept everything would be a second copy of a database it can ask
   * at any time.  Scrolling past the window is a CHATHISTORY away.
   */
  keep = 500;

  /** Canonical form of a nickname or channel, for keying.
   *
   * RFC 1459 casemapping, which is what this server uses: `[`, `]` and
   * `\` are the capitals of `{`, `}` and `|`.  Using `toLowerCase()`
   * would make `nick[home]` and `nick{home}` different people.
   */
  static fold(name: string): string {
    let out = '';

    for (const ch of name) {
      if (ch >= 'A' && ch <= 'Z') out += ch.toLowerCase();
      else if (ch === '[') out += '{';
      else if (ch === ']') out += '}';
      else if (ch === '\\') out += '|';
      else if (ch === '^') out += '~';
      else out += ch;
    }

    return out;
  }

  /** Is this a channel name? */
  static isChannel(name: string): boolean {
    return name.startsWith('#') || name.startsWith('&');
  }

  channel(name: string): Channel | undefined {
    return this.channels.get(NetworkState.fold(name));
  }

  user(nick: string): User | undefined {
    return this.users.get(NetworkState.fold(nick));
  }

  /** Find or create a user record. */
  touchUser(nick: string): User {
    const key = NetworkState.fold(nick);
    const found = this.users.get(key);

    if (found) return found;

    const made: User = { nick, identified: false, frozen: false };

    this.users.set(key, made);

    return made;
  }

  /** Find or create a channel record. */
  touchChannel(name: string): Channel {
    const key = NetworkState.fold(name);
    const found = this.channels.get(key);

    if (found) return found;

    const made: Channel = {
      name,
      modes: new Set(),
      members: new Map(),
      joining: true,
    };

    this.channels.set(key, made);

    return made;
  }

  /** A nickname changed: move every record that is keyed by it. */
  renameUser(from: string, to: string): void {
    const oldKey = NetworkState.fold(from);
    const user = this.users.get(oldKey);

    this.users.delete(oldKey);

    if (user) {
      // The account is left alone: it is not the nickname, and nothing
      // about changing one says anything about the other.
      user.nick = to;
      this.users.set(NetworkState.fold(to), user);
    }

    for (const channel of this.channels.values()) {
      const member = channel.members.get(oldKey);

      if (!member) continue;

      channel.members.delete(oldKey);
      channel.members.set(NetworkState.fold(to), { ...member, nick: to });
    }

    if (NetworkState.fold(this.nick) === oldKey) this.nick = to;
  }

  /** Somebody left the network. */
  removeUser(nick: string): void {
    const key = NetworkState.fold(nick);

    this.users.delete(key);

    for (const channel of this.channels.values()) channel.members.delete(key);
  }

  /** Keep a message, oldest dropped past {@link keep}. */
  store(target: string, msg: StoredMessage): void {
    const key = NetworkState.fold(target);
    const list = this.messages.get(key) ?? [];

    // A message may arrive twice: once off the wire and once from a
    // CHATHISTORY the user scrolled into.  The server gives every
    // message one network-wide name precisely so this is decidable.
    if (msg.id && list.some((m) => m.id === msg.id)) return;

    list.push(msg);

    // Replayed history arrives newest-first or oldest-first depending on
    // the shape asked for, so the list is sorted rather than assumed.
    if (list.length > 1) {
      const prev = list[list.length - 2] as StoredMessage;

      if (prev.at.getTime() > msg.at.getTime()) {
        list.sort((a, b) => a.at.getTime() - b.at.getTime());
      }
    }

    if (list.length > this.keep) list.splice(0, list.length - this.keep);

    this.messages.set(key, list);
  }

  /** What is kept for a target, oldest first. */
  history(target: string): readonly StoredMessage[] {
    return this.messages.get(NetworkState.fold(target)) ?? [];
  }

  /** Mark a message redacted, keeping the row.
   * @returns Whether it was found.
   */
  redact(target: string, id: string): boolean {
    for (const msg of this.messages.get(NetworkState.fold(target)) ?? []) {
      if (msg.id !== id) continue;

      msg.redacted = true;
      return true;
    }

    return false;
  }

  /** Change what a message says, keeping which message it is.
   * @returns Whether it was found.
   */
  edit(target: string, id: string, text: string, at: Date): boolean {
    for (const msg of this.messages.get(NetworkState.fold(target)) ?? []) {
      if (msg.id !== id) continue;

      msg.text = text;
      msg.edited = at;
      return true;
    }

    return false;
  }

  /** How many messages in a target are past its read marker. */
  unread(target: string): number {
    const marker = this.readMarkers.get(NetworkState.fold(target));

    if (!marker) return this.history(target).length;

    let n = 0;

    for (const msg of this.history(target)) {
      if (msg.at.getTime() > marker.getTime() && msg.from !== this.nick) n++;
    }

    return n;
  }

  /** Forget everything.  For a connection that is starting over. */
  reset(): void {
    this.channels.clear();
    this.users.clear();
    this.modes.clear();
    this.isupport.clear();
    this.identified = false;
    this.account = undefined;
    this.frozen = false;
    // Messages and read markers are *not* cleared: they are what the
    // user was reading a moment ago, and a reconnection is not a reason
    // to blank the screen.
  }
}
