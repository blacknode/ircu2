import { describe, expect, test } from 'bun:test';

import {
  chatHistoryCommand,
  editCommand,
  searchCommand,
} from '../src/conversation.js';
import { parseMessage } from '../src/message.js';

describe('searchCommand', () => {
  test('what to look for is the last parameter, so it may be a sentence', () => {
    const msg = parseMessage(searchCommand('#tea', 'the marmalade is where'))!;

    expect(msg.command).toBe('SEARCH');
    expect(msg.params[0]).toBe('#tea');
    expect(msg.params[msg.params.length - 1]).toBe('the marmalade is where');

    // Which means it survives the round trip whole, rather than arriving
    // as four parameters the server would read as options.
    expect(msg.params.length).toBe(2);
  });

  test('the options go between the target and the text', () => {
    const line = searchCommand('*', 'wombat', {
      from: 'maria',
      limit: 5,
      after: new Date('2026-01-01T00:00:00.000Z'),
    });
    const msg = parseMessage(line)!;

    expect(msg.params[0]).toBe('*');
    expect(msg.params.slice(1, -1)).toEqual([
      'from=maria',
      'after=2026-01-01T00:00:00.000Z',
      'limit=5',
    ]);
    expect(msg.params[msg.params.length - 1]).toBe('wombat');
  });

  test('a search with no options is a target and a sentence', () => {
    const msg = parseMessage(searchCommand('maria', 'it is what it is'))!;

    expect(msg.params).toEqual(['maria', 'it is what it is']);
  });
});

describe('editCommand', () => {
  test('the identifier is what an edit names, and it does not change', () => {
    const msg = parseMessage(editCommand('#tea', 'ABC123', 'meet at seven'))!;

    expect(msg.command).toBe('EDIT');
    expect(msg.params).toEqual(['#tea', 'ABC123', 'meet at seven']);
  });
});

describe('chatHistoryCommand', () => {
  test('TARGETS is the one that takes no target', () => {
    const msg = parseMessage(
      chatHistoryCommand('ignored', {
        shape: 'TARGETS',
        from: { kind: 'timestamp', at: new Date('2026-01-01T00:00:00.000Z') },
        to: { kind: 'latest' },
        limit: 10,
      }),
    )!;

    expect(msg.params[0]).toBe('TARGETS');
    expect(msg.params[1]).toBe('timestamp=2026-01-01T00:00:00.000Z');
    expect(msg.params[2]).toBe('*');
  });
});
