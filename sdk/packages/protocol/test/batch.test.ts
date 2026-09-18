import { describe, expect, test } from 'bun:test';

import { BatchAssembler, joinMultiline, splitMultiline } from '../src/batch.js';
import { parseMessage } from '../src/message.js';

function feed(a: BatchAssembler, line: string) {
  return a.handle(parseMessage(line)!);
}

describe('BatchAssembler', () => {
  test('a line outside a batch goes straight through', () => {
    const a = new BatchAssembler();
    const out = feed(a, ':bob!u@h PRIVMSG #c :hi');

    expect(out.deliver.length).toBe(1);
    expect(out.deliver[0]!.params[1]).toBe('hi');
  });

  test('lines inside a batch are held and then delivered together', () => {
    const a = new BatchAssembler();

    expect(feed(a, 'BATCH +abc chathistory #chan').deliver).toEqual([]);
    expect(feed(a, '@batch=abc :bob!u@h PRIVMSG #chan :one').deliver).toEqual([]);
    expect(feed(a, '@batch=abc :bob!u@h PRIVMSG #chan :two').deliver).toEqual([]);

    const out = feed(a, 'BATCH -abc');

    expect(out.deliver.length).toBe(2);
    expect(out.closed!.type).toBe('chathistory');
    expect(out.closed!.params).toEqual(['#chan']);
    expect(a.pending).toBe(0);
  });

  test('an empty batch closes with nothing, which is a complete answer', () => {
    // The server opens a batch lazily, on the first message a command
    // actually sends -- so "none arrived" is a real outcome and not a
    // dropped connection.
    const a = new BatchAssembler();

    feed(a, 'BATCH +abc chathistory #chan');
    const out = feed(a, 'BATCH -abc');

    expect(out.deliver).toEqual([]);
    expect(out.closed!.messages).toEqual([]);
  });

  test('a nested batch is folded into its parent', () => {
    const a = new BatchAssembler();

    feed(a, 'BATCH +outer labeled-response');
    feed(a, '@batch=outer BATCH +inner chathistory #chan');
    feed(a, '@batch=inner :bob!u@h PRIVMSG #chan :held');
    feed(a, '@batch=outer BATCH -inner');

    // The inner batch closed, but its lines belong to the outer one and
    // are not delivered yet.
    expect(a.pending).toBe(1);

    const out = feed(a, 'BATCH -outer');

    expect(out.deliver.length).toBe(1);
    expect(out.deliver[0]!.params[1]).toBe('held');
  });

  test('a line naming a batch we never saw is delivered, not dropped', () => {
    // Losing a message because its bookkeeping was odd is worse than
    // showing it out of order.
    const a = new BatchAssembler();
    const out = feed(a, '@batch=ghost :bob!u@h PRIVMSG #c :hi');

    expect(out.deliver.length).toBe(1);
  });

  test('closing a batch that was never opened is quietly nothing', () => {
    expect(feed(new BatchAssembler(), 'BATCH -ghost').deliver).toEqual([]);
  });

  test('reset() forgets a connection that died mid-batch', () => {
    const a = new BatchAssembler();

    feed(a, 'BATCH +abc chathistory #chan');
    expect(a.pending).toBe(1);

    a.reset();
    expect(a.pending).toBe(0);
  });
});

describe('joinMultiline', () => {
  test('pieces become lines', () => {
    const a = new BatchAssembler();

    feed(a, 'BATCH +m draft/multiline #chan');
    feed(a, '@batch=m :bob!u@h PRIVMSG #chan :first');
    feed(a, '@batch=m :bob!u@h PRIVMSG #chan :second');

    const out = feed(a, 'BATCH -m');

    expect(joinMultiline(out.closed!)).toBe('first\nsecond');
  });

  test('a concat piece continues the line rather than starting one', () => {
    // How a long paragraph with no newlines crosses a 512-byte wire and
    // arrives with none.
    const a = new BatchAssembler();

    feed(a, 'BATCH +m draft/multiline #chan');
    feed(a, '@batch=m :bob!u@h PRIVMSG #chan :one long ');
    feed(a, '@batch=m;draft/multiline-concat :bob!u@h PRIVMSG #chan :paragraph');

    const out = feed(a, 'BATCH -m');

    expect(joinMultiline(out.closed!)).toBe('one long paragraph');
  });

  test('a batch of another type is not one', () => {
    const a = new BatchAssembler();

    feed(a, 'BATCH +m chathistory #chan');
    const out = feed(a, 'BATCH -m');

    expect(joinMultiline(out.closed!)).toBeUndefined();
  });
});

describe('splitMultiline', () => {
  const limits = { maxBytes: 4096, maxLines: 24 };

  test('newlines become pieces', () => {
    expect(splitMultiline('a\nb\nc', 30, limits)).toEqual([
      { text: 'a', concat: false },
      { text: 'b', concat: false },
      { text: 'c', concat: false },
    ]);
  });

  test('a line too long for the wire is broken and marked as continued', () => {
    const pieces = splitMultiline('x'.repeat(300), 0, limits, 100);

    expect(pieces.length).toBe(3);
    expect(pieces[0]!.concat).toBe(false);
    expect(pieces[1]!.concat).toBe(true);
    expect(pieces[2]!.concat).toBe(true);
  });

  test('it round-trips through the reassembler', () => {
    const text = 'short line\n' + 'y'.repeat(250) + '\nlast';
    const pieces = splitMultiline(text, 0, limits, 100);
    const a = new BatchAssembler();

    feed(a, 'BATCH +m draft/multiline #chan');
    for (const p of pieces) {
      const tags = p.concat ? '@batch=m;draft/multiline-concat' : '@batch=m';

      feed(a, `${tags} :bob!u@h PRIVMSG #chan :${p.text}`);
    }
    const out = feed(a, 'BATCH -m');

    expect(joinMultiline(out.closed!)).toBe(text);
  });

  test('past the server’s line limit it gives up rather than truncating', () => {
    // The server would refuse the batch, so the caller has to fall back
    // to separate messages; saying so with an empty array keeps that
    // decision where it belongs.
    const text = Array.from({ length: 40 }, (_, i) => `line ${i}`).join('\n');

    expect(splitMultiline(text, 0, limits)).toEqual([]);
  });

  test('past the byte limit, likewise', () => {
    expect(splitMultiline('x'.repeat(5000), 0, { maxBytes: 4096, maxLines: 99 }, 500)).toEqual([]);
  });
});
