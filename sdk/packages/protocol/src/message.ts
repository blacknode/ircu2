/**
 * One line of IRC, both directions.
 *
 * This file is the whole of the wire format and it is deliberately the
 * dullest thing in the SDK: a parser everything else depends on is the
 * one place where being nearly right is a bug that surfaces somewhere
 * else entirely.
 *
 * It is pure -- no I/O, no state, no clock -- which is what lets it be
 * tested exhaustively, the same split `ircd/msgid.c` has from the server
 * it runs in.
 */

/** A message tag, as it arrived. */
export interface Tags {
  readonly [key: string]: string;
}

/** Who a line says it is from.
 *
 * On this server a line to a client always carries the long form, so
 * `nick` is the nickname and `user`/`host` are present for a user and
 * absent for a server.  The raw form is kept because a client that wants
 * to echo a prefix back should echo what it was given.
 */
export interface Prefix {
  readonly raw: string;
  readonly nick: string;
  readonly user?: string;
  readonly host?: string;
  /** A prefix with no `!` and no `@` is a server, not a nickname. */
  readonly isServer: boolean;
}

/** A parsed line. */
export interface Message {
  readonly tags: Tags;
  readonly prefix?: Prefix;
  /** Uppercased: `PRIVMSG`, `CAP`, `001`.  Numerics keep their digits. */
  readonly command: string;
  readonly params: readonly string[];
  /** The line as it arrived, without the CR LF. */
  readonly raw: string;
}

/** What `formatMessage` is given. */
export interface OutgoingMessage {
  readonly tags?: Record<string, string | true> | undefined;
  readonly command: string;
  readonly params?: readonly string[] | undefined;
}

/** The longest line this server will read, excluding CR LF.
 *
 * 512 is RFC 1459's, and it is the body only: tags are extra and have
 * their own limit, which is why they are counted separately below.
 */
export const MAX_BODY_BYTES = 512;

/** The longest tag section a client may send, including the leading `@`. */
export const MAX_CLIENT_TAG_BYTES = 4096;

const TAG_ESCAPES: Record<string, string> = {
  ':': ';',
  s: ' ',
  '\\': '\\',
  r: '\r',
  n: '\n',
};

const TAG_UNESCAPES: Record<string, string> = {
  ';': '\\:',
  ' ': '\\s',
  '\\': '\\\\',
  '\r': '\\r',
  '\n': '\\n',
};

/** Undo the escaping in a tag value (IRCv3 message-tags). */
export function unescapeTagValue(value: string): string {
  let out = '';

  for (let i = 0; i < value.length; i++) {
    if (value[i] !== '\\') {
      out += value[i];
      continue;
    }

    const next = value[i + 1];

    // A trailing lone backslash is dropped, and an unrecognised escape is
    // the character itself.  Both are what the spec says, and both are
    // cases a hand-written parser gets wrong by throwing instead.
    if (next === undefined) break;

    out += TAG_ESCAPES[next] ?? next;
    i++;
  }

  return out;
}

/** Apply it. */
export function escapeTagValue(value: string): string {
  let out = '';

  for (const ch of value) out += TAG_UNESCAPES[ch] ?? ch;

  return out;
}

/** Parse the tag section, without its leading `@`. */
function parseTags(section: string): Tags {
  const tags: Record<string, string> = {};

  for (const item of section.split(';')) {
    if (!item) continue;

    const eq = item.indexOf('=');

    if (eq < 0) {
      // A tag with no `=` and one with `=` and nothing after it are the
      // same thing to a reader: present, with no value.
      tags[item] = '';
      continue;
    }

    tags[item.slice(0, eq)] = unescapeTagValue(item.slice(eq + 1));
  }

  return tags;
}

/** Parse a prefix, without its leading `:`. */
function parsePrefix(raw: string): Prefix {
  const bang = raw.indexOf('!');
  const at = raw.indexOf('@');

  if (bang > 0 && at > bang) {
    return {
      raw,
      nick: raw.slice(0, bang),
      user: raw.slice(bang + 1, at),
      host: raw.slice(at + 1),
      isServer: false,
    };
  }

  if (at > 0) {
    return { raw, nick: raw.slice(0, at), host: raw.slice(at + 1), isServer: false };
  }

  // No `!` and no `@`.  On this server that is a server name -- every
  // line to a client carries the long form -- but a nickname-only prefix
  // is legal IRC, so this says "server" by shape and lets the caller
  // decide what to do about it.
  return { raw, nick: raw, isServer: raw.includes('.') };
}

/**
 * Parse one line.
 *
 * @param line One line, with or without its trailing CR LF.
 * @returns The message, or `undefined` for a line with no command --
 *   which is what a keep-alive empty line is, and is not an error.
 */
export function parseMessage(line: string): Message | undefined {
  const raw = line.replace(/\r?\n$/, '');
  let rest = raw;
  let tags: Tags = {};
  let prefix: Prefix | undefined;

  if (rest.startsWith('@')) {
    const sp = rest.indexOf(' ');
    if (sp < 0) return undefined;

    tags = parseTags(rest.slice(1, sp));
    rest = rest.slice(sp + 1).replace(/^ +/, '');
  }

  if (rest.startsWith(':')) {
    const sp = rest.indexOf(' ');
    if (sp < 0) return undefined;

    prefix = parsePrefix(rest.slice(1, sp));
    rest = rest.slice(sp + 1).replace(/^ +/, '');
  }

  if (!rest) return undefined;

  const params: string[] = [];
  let command = '';

  while (rest.length) {
    if (rest.startsWith(':')) {
      // The trailing parameter: everything left, spaces and all, and it
      // is the only one that may be empty or start with a colon.
      params.push(rest.slice(1));
      break;
    }

    const sp = rest.indexOf(' ');
    const word = sp < 0 ? rest : rest.slice(0, sp);

    if (!command) command = word;
    else params.push(word);

    if (sp < 0) break;

    rest = rest.slice(sp + 1).replace(/^ +/, '');
  }

  if (!command) return undefined;

  return {
    tags,
    ...(prefix ? { prefix } : {}),
    command: command.toUpperCase(),
    params,
    raw,
  };
}

/**
 * Build one line, without its trailing CR LF.
 *
 * The last parameter is written as a trailing parameter when it has to
 * be -- it is empty, it holds a space, or it starts with a colon -- and
 * not otherwise, so a line built here is byte-identical to what a
 * traditional client would have sent.  That matters more than it looks:
 * this server's compatibility story is that a client which negotiated
 * nothing sees exactly the lines it always saw, and an SDK that added a
 * colon everywhere would be the first thing to break the symmetry.
 */
export function formatMessage(msg: OutgoingMessage): string {
  let out = '';

  if (msg.tags) {
    const parts: string[] = [];

    for (const [key, value] of Object.entries(msg.tags)) {
      if (value === true || value === '') parts.push(key);
      else parts.push(`${key}=${escapeTagValue(value)}`);
    }

    if (parts.length) out += `@${parts.join(';')} `;
  }

  out += msg.command;

  const params = msg.params ?? [];

  for (let i = 0; i < params.length; i++) {
    const p = params[i] as string;
    const last = i === params.length - 1;

    if (last && (p === '' || p.includes(' ') || p.startsWith(':'))) out += ` :${p}`;
    else out += ` ${p}`;
  }

  return out;
}

/** How many bytes a string takes on the wire.
 *
 * UTF-8, because that is what this server's lines are, and because a
 * length check done in UTF-16 code units silently allows a line that is
 * too long the moment somebody types an emoji.
 */
export function byteLength(text: string): number {
  return new TextEncoder().encode(text).length;
}

/**
 * Split `text` so that each piece fits in one PRIVMSG to `target`.
 *
 * Splits on a space where there is one near the end, and mid-word only
 * when there is not -- a word longer than a line has to break somewhere.
 * Never splits inside a UTF-8 sequence, because half a character is not
 * a character.
 *
 * Used when `draft/multiline` is *not* available.  With it, the whole
 * message goes as one batch and the server reassembles; see `batch.ts`.
 */
export function splitForLine(
  text: string,
  overheadBytes: number,
  limit = MAX_BODY_BYTES,
): string[] {
  const room = limit - overheadBytes;

  if (room <= 0) return [text];
  if (byteLength(text) <= room) return [text];

  const out: string[] = [];
  let current = '';
  let currentBytes = 0;

  const flush = () => {
    if (current) out.push(current);
    current = '';
    currentBytes = 0;
  };

  // Iterating the string gives whole code points, so a surrogate pair is
  // never cut in half.
  for (const ch of text) {
    const chBytes = byteLength(ch);

    if (currentBytes + chBytes > room) {
      const lastSpace = current.lastIndexOf(' ');

      // Only break at a space if it is near enough the end to be worth
      // it; otherwise a long URL would push a whole line onto the next
      // piece and leave this one half empty.
      if (lastSpace > 0 && lastSpace > current.length - 24) {
        const head = current.slice(0, lastSpace);
        const tail = current.slice(lastSpace + 1);

        out.push(head);
        current = tail;
        currentBytes = byteLength(tail);
      } else {
        flush();
      }
    }

    current += ch;
    currentBytes += chBytes;
  }

  flush();

  return out;
}
