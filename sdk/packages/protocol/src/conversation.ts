/**
 * The conversation layer's wire details: tags, CHATHISTORY, standard
 * replies.
 *
 * Phase 3 and 4 of the roadmap, as seen from a client.  Still pure --
 * what belongs here is the exact spelling of a thing, so that the
 * spelling lives in one place and not in five call sites.
 */

import { formatMessage } from './message.js';

/**
 * "This message follows that one."
 *
 * A client tag, so it carries a leading `+` on the wire, and it is the
 * client's claim rather than anything the server checks: a reply may
 * name a message the retention already dropped.
 */
export const TAG_REPLY = '+draft/reply';

/**
 * "This is a reaction to that one."
 *
 * A reaction is a TAGMSG that carries both this and {@link TAG_REPLY}:
 * the reply tag says what it is about, this one says it is a reaction
 * rather than a message.  The server stores it as a message of its own,
 * which is what makes removing one and reading them back in order the
 * same operations as for anything else.
 */
export const TAG_REACT = '+draft/react';

/** "I am typing."  Never stored: a row saying somebody was typing in
 * March is not history. */
export const TAG_TYPING = '+typing';

/**
 * "This body is Markdown."
 *
 * Sent with a message when `blacknode/richtext` is in force.  Every
 * client that did not negotiate it gets the plain-text equivalent the
 * *server* generated, and this tag is suppressed on that copy -- a tag
 * saying the body is Markdown would be a lie on the other one.
 */
export const TAG_FORMAT = '+blacknode/format';

/** The one value {@link TAG_FORMAT} takes today. */
export const FORMAT_MARKDOWN = 'markdown';

/** A FAIL, WARN or NOTE (IRCv3 `standard-replies`). */
export interface StandardReply {
  readonly kind: 'FAIL' | 'WARN' | 'NOTE';
  /** The command it is about, or `*`. */
  readonly command: string;
  /** A machine-readable code: `ACCOUNT_REQUIRED`, `INVALID_TARGET`, ... */
  readonly code: string;
  /** Context words between the code and the text; usually empty. */
  readonly context: readonly string[];
  readonly text: string;
}

/** Read one, or undefined when the message is not one. */
export function parseStandardReply(
  command: string,
  params: readonly string[],
): StandardReply | undefined {
  if (command !== 'FAIL' && command !== 'WARN' && command !== 'NOTE') return undefined;

  // <command> <code> [context...] :<description>
  if (params.length < 3) return undefined;

  return {
    kind: command,
    command: params[0] as string,
    code: params[1] as string,
    context: params.slice(2, -1),
    text: params[params.length - 1] as string,
  };
}

/** Which slice of a conversation to ask for.
 *
 * The shapes the server implements (`hist_read.c`).  `BETWEEN` is
 * symmetric on purpose -- the server does not care which of the two
 * points is the earlier -- so a caller need not sort them first.
 */
export type ChatHistorySelector =
  | { readonly shape: 'LATEST'; readonly limit: number }
  | { readonly shape: 'BEFORE' | 'AFTER' | 'AROUND'; readonly at: HistoryPoint; readonly limit: number }
  | {
      readonly shape: 'BETWEEN';
      readonly from: HistoryPoint;
      readonly to: HistoryPoint;
      readonly limit: number;
    }
  | { readonly shape: 'TARGETS'; readonly from: HistoryPoint; readonly to: HistoryPoint; readonly limit: number };

/** A point in a conversation: a moment, or a message.
 *
 * A timestamp is the honest one for "everything since I was last here":
 * a message that arrived late from a netsplit is still *behind* the
 * marker if it was sent behind it, which a msgid cannot express.
 */
export type HistoryPoint =
  | { readonly kind: 'timestamp'; readonly at: Date }
  | { readonly kind: 'msgid'; readonly id: string }
  | { readonly kind: 'latest' };

function point(p: HistoryPoint): string {
  switch (p.kind) {
    case 'timestamp':
      return `timestamp=${p.at.toISOString().replace(/\.(\d{3})Z$/, '.$1Z')}`;
    case 'msgid':
      return `msgid=${p.id}`;
    case 'latest':
      return '*';
  }
}

/**
 * Build a CHATHISTORY line.
 *
 * @param target A channel, or the nickname of the other end of a direct
 *   message.  On this server a direct message is filed under the
 *   nickname both ends *proved* -- an account is a nickname -- so a
 *   conversation with somebody who never identified is not in the store
 *   to ask about.
 */
export function chatHistoryCommand(target: string, sel: ChatHistorySelector): string {
  switch (sel.shape) {
    case 'LATEST':
      return formatMessage({
        command: 'CHATHISTORY',
        params: ['LATEST', target, '*', String(sel.limit)],
      });

    case 'BEFORE':
    case 'AFTER':
    case 'AROUND':
      return formatMessage({
        command: 'CHATHISTORY',
        params: [sel.shape, target, point(sel.at), String(sel.limit)],
      });

    case 'BETWEEN':
      return formatMessage({
        command: 'CHATHISTORY',
        params: ['BETWEEN', target, point(sel.from), point(sel.to), String(sel.limit)],
      });

    case 'TARGETS':
      // TARGETS is the one that does not take a target: it answers with
      // the conversations themselves, which is what an inbox is built
      // from.
      return formatMessage({
        command: 'CHATHISTORY',
        params: ['TARGETS', point(sel.from), point(sel.to), String(sel.limit)],
      });
  }
}
