/**
 * Batches, labeled responses and multiline, on the receiving side.
 *
 * Three IRCv3 features that all mean "these lines belong together", and
 * one place that turns them back into single events so nothing above has
 * to know they happened.
 *
 * The server opens a batch **lazily** -- on the first message a command
 * actually sends -- so nothing here may assume it knows how many lines
 * are coming, or that any are.  A labeled request that produces nothing
 * comes back as a bare `ACK`, and that is a complete answer.
 */

import type { Message, Tags } from './message.js';

/** One batch being collected. */
export interface OpenBatch {
  readonly id: string;
  readonly type: string;
  readonly params: readonly string[];
  /** The tags on the *opening* line.
   *
   * Which is where `label` rides, and where a multiline message's one
   * `msgid` is -- the pieces carry none.  Both are on the line that
   * opens the batch and not on anything inside it, so a caller that
   * looked at the first message would find neither.
   */
  readonly tags: Tags;
  /** The batch this one is nested inside, if any. */
  readonly parent?: string;
  /** Lines collected so far, in order. */
  readonly messages: Message[];
}

/** What came out of feeding one line in. */
export interface BatchEvent {
  /** Lines to hand to the rest of the client, in order.
   *
   * A line inside a batch is held until the batch closes, so this is
   * empty for most of a batch's life and then holds all of it.
   */
  readonly deliver: readonly Message[];
  /** Set when a batch just closed; the caller may want it whole. */
  readonly closed?: OpenBatch;
}

const NOTHING: BatchEvent = { deliver: [] };

/**
 * Reassembles batches.
 *
 * Feed it every line.  It answers with the lines to act on: outside a
 * batch that is the line itself, and inside one it is nothing until the
 * batch closes and then everything at once.
 *
 * Multiline is the exception and is handled here rather than above,
 * because a `draft/multiline` batch is not several messages that belong
 * together -- it is *one* message the wire could not carry in one line,
 * and anything above that treated it as several would show a paragraph
 * as a burst.
 */
export class BatchAssembler {
  private readonly open = new Map<string, OpenBatch>();

  /** The batch a line belongs to, or undefined. */
  private batchOf(msg: Message): string | undefined {
    const id = msg.tags['batch'];

    return id ? id : undefined;
  }

  /** Feed one line. */
  handle(msg: Message): BatchEvent {
    if (msg.command === 'BATCH') return this.onBatch(msg);

    const id = this.batchOf(msg);

    if (!id) return { deliver: [msg] };

    const batch = this.open.get(id);

    // A line naming a batch we never saw opened.  Delivered rather than
    // dropped: losing a message because its bookkeeping was odd is worse
    // than showing it out of order.
    if (!batch) return { deliver: [msg] };

    batch.messages.push(msg);

    return NOTHING;
  }

  /** BATCH +id type [params] / BATCH -id. */
  private onBatch(msg: Message): BatchEvent {
    const ref = msg.params[0] ?? '';

    if (ref.startsWith('+')) {
      const id = ref.slice(1);
      const parent = this.batchOf(msg);

      this.open.set(id, {
        id,
        type: msg.params[1] ?? '',
        params: msg.params.slice(2),
        tags: msg.tags,
        ...(parent ? { parent } : {}),
        messages: [],
      });

      // The opening line itself carries the msgid of a multiline
      // message, so it is kept with the batch rather than delivered.
      return NOTHING;
    }

    if (!ref.startsWith('-')) return { deliver: [msg] };

    const id = ref.slice(1);
    const batch = this.open.get(id);

    if (!batch) return NOTHING;

    this.open.delete(id);

    // A batch that closes inside another one belongs to that one.
    if (batch.parent) {
      const parent = this.open.get(batch.parent);

      if (parent) {
        for (const m of batch.messages) parent.messages.push(m);
        return { closed: batch, deliver: [] };
      }
    }

    return { closed: batch, deliver: batch.messages };
  }

  /** Forget everything.  For a connection that went away mid-batch. */
  reset(): void {
    this.open.clear();
  }

  /** Batches still open, for diagnostics. */
  get pending(): number {
    return this.open.size;
  }
}

/**
 * Join the pieces of a `draft/multiline` batch back into one message.
 *
 * Each piece is a PRIVMSG or NOTICE; a piece tagged
 * `draft/multiline-concat` continues the previous line instead of
 * starting a new one, which is how a long paragraph with no newlines in
 * it crosses a 512-byte wire and arrives with none.
 *
 * @returns The joined text, or undefined when the batch is not one.
 */
export function joinMultiline(batch: OpenBatch): string | undefined {
  if (batch.type !== 'draft/multiline') return undefined;

  let out = '';
  let first = true;

  for (const msg of batch.messages) {
    if (msg.command !== 'PRIVMSG' && msg.command !== 'NOTICE') continue;

    const text = msg.params[1] ?? '';
    const concat = 'draft/multiline-concat' in msg.tags;

    if (first) out = text;
    else out += (concat ? '' : '\n') + text;

    first = false;
  }

  return out;
}

/** Split a message for sending as a multiline batch.
 *
 * The inverse of {@link joinMultiline}: a line of the text becomes a
 * piece, and a line too long for the wire becomes several pieces with
 * `draft/multiline-concat` on all but the first -- so that what the
 * recipient reassembles is exactly what was typed, newlines and all.
 *
 * @returns The pieces, each with a flag saying whether it continues the
 *   one before it.  Empty when the message fits in one ordinary line.
 */
export function splitMultiline(
  text: string,
  overheadBytes: number,
  limits: { maxBytes: number; maxLines: number },
  lineLimit = 512,
): { text: string; concat: boolean }[] {
  const pieces: { text: string; concat: boolean }[] = [];
  const room = Math.max(1, lineLimit - overheadBytes);
  const encoder = new TextEncoder();

  for (const line of text.split('\n')) {
    if (encoder.encode(line).length <= room) {
      pieces.push({ text: line, concat: false });
      continue;
    }

    // A single line too long for the wire: break it, and mark every
    // piece after the first as a continuation so it is rejoined without
    // a newline that was never typed.
    let current = '';
    let bytes = 0;
    let firstPiece = true;

    for (const ch of line) {
      const n = encoder.encode(ch).length;

      if (bytes + n > room) {
        pieces.push({ text: current, concat: !firstPiece });
        firstPiece = false;
        current = '';
        bytes = 0;
      }

      current += ch;
      bytes += n;
    }

    if (current) pieces.push({ text: current, concat: !firstPiece });
  }

  // Past the server's limits it would refuse the batch, so the caller has
  // to fall back to separate messages.  Saying so with an empty array
  // rather than a truncated batch keeps the decision where it belongs.
  if (pieces.length > limits.maxLines) return [];

  const total = pieces.reduce((n, p) => n + encoder.encode(p.text).length, 0);

  if (limits.maxBytes && total > limits.maxBytes) return [];

  return pieces;
}
