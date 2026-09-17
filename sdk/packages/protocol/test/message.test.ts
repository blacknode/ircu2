import { describe, expect, test } from 'bun:test';

import {
  byteLength,
  escapeTagValue,
  formatMessage,
  parseMessage,
  splitForLine,
  unescapeTagValue,
} from '../src/message.js';

describe('parseMessage', () => {
  test('a bare command', () => {
    const m = parseMessage('PING')!;

    expect(m.command).toBe('PING');
    expect(m.params).toEqual([]);
    expect(m.prefix).toBeUndefined();
    expect(m.tags).toEqual({});
  });

  test('the command is uppercased and a numeric keeps its digits', () => {
    expect(parseMessage('privmsg #a :hi')!.command).toBe('PRIVMSG');
    expect(parseMessage(':irc.example.net 001 bob :Welcome')!.command).toBe('001');
  });

  test('a full line', () => {
    const m = parseMessage(':bob!u@host PRIVMSG #chan :hello world')!;

    expect(m.prefix).toEqual({
      raw: 'bob!u@host',
      nick: 'bob',
      user: 'u',
      host: 'host',
      isServer: false,
    });
    expect(m.params).toEqual(['#chan', 'hello world']);
  });

  test('a server prefix has no user and says so', () => {
    const m = parseMessage(':irc.example.net NOTICE bob :hi')!;

    expect(m.prefix!.isServer).toBe(true);
    expect(m.prefix!.user).toBeUndefined();
  });

  test('the trailing parameter keeps its spaces and may be empty', () => {
    expect(parseMessage('PRIVMSG #a :  two  spaces  ')!.params[1]).toBe('  two  spaces  ');
    expect(parseMessage('PRIVMSG #a :')!.params[1]).toBe('');
  });

  test('a trailing parameter may start with a colon', () => {
    expect(parseMessage('PRIVMSG #a ::-)')!.params[1]).toBe(':-)');
  });

  test('tags, with and without values', () => {
    const m = parseMessage('@a=1;b;c=;msgid=xyz PING')!;

    expect(m.tags).toEqual({ a: '1', b: '', c: '', msgid: 'xyz' });
  });

  test('tag escapes', () => {
    const m = parseMessage('@x=a\\:b\\sc\\\\d\\r\\ne PING')!;

    expect(m.tags['x']).toBe('a;b c\\d\r\ne');
  });

  test('an unknown escape is the character itself, and a lone backslash goes', () => {
    expect(unescapeTagValue('a\\qb')).toBe('aqb');
    expect(unescapeTagValue('trail\\')).toBe('trail');
  });

  test('escaping round-trips', () => {
    for (const value of ['plain', 'a;b', 'a b', 'a\\b', 'a\rb', 'a\nb', '; \\\r\n']) {
      expect(unescapeTagValue(escapeTagValue(value))).toBe(value);
    }
  });

  test('client tags keep their plus', () => {
    const m = parseMessage('@+draft/reply=abc TAGMSG #chan')!;

    expect(m.tags['+draft/reply']).toBe('abc');
  });

  test('tags and a prefix together', () => {
    const m = parseMessage('@time=2026-01-01T00:00:00.000Z :bob!u@h PRIVMSG #c :hi')!;

    expect(m.tags['time']).toBe('2026-01-01T00:00:00.000Z');
    expect(m.prefix!.nick).toBe('bob');
    expect(m.params).toEqual(['#c', 'hi']);
  });

  test('the trailing CR LF is not part of anything', () => {
    expect(parseMessage('PRIVMSG #a :hi\r\n')!.params[1]).toBe('hi');
    expect(parseMessage('PRIVMSG #a :hi\r\n')!.raw).toBe('PRIVMSG #a :hi');
  });

  test('a line with no command is undefined rather than a throw', () => {
    // A server may send an empty line as a keep-alive; every one of
    // these is a thing a socket can deliver and none is an error worth
    // tearing a connection down for.
    expect(parseMessage('')).toBeUndefined();
    expect(parseMessage('   ')).toBeUndefined();
    expect(parseMessage('@only=tags')).toBeUndefined();
    expect(parseMessage(':only.prefix')).toBeUndefined();
  });

  test('runs of spaces between parameters are not empty parameters', () => {
    expect(parseMessage('PRIVMSG   #a    :hi')!.params).toEqual(['#a', 'hi']);
  });
});

describe('formatMessage', () => {
  test('a parameter is only made trailing when it has to be', () => {
    // The compatibility story: a client that negotiated nothing sees the
    // lines it always saw, and a colon added everywhere would be the
    // first thing to break the symmetry.
    expect(formatMessage({ command: 'JOIN', params: ['#chan'] })).toBe('JOIN #chan');
    expect(formatMessage({ command: 'PRIVMSG', params: ['#c', 'hi'] })).toBe('PRIVMSG #c hi');
    expect(formatMessage({ command: 'PRIVMSG', params: ['#c', 'a b'] })).toBe('PRIVMSG #c :a b');
    expect(formatMessage({ command: 'PRIVMSG', params: ['#c', ''] })).toBe('PRIVMSG #c :');
    expect(formatMessage({ command: 'PRIVMSG', params: ['#c', ':-)'] })).toBe('PRIVMSG #c ::-)');
  });

  test('tags, valued and valueless', () => {
    expect(formatMessage({ tags: { a: '1', b: true }, command: 'PING' })).toBe('@a=1;b PING');
    expect(formatMessage({ tags: { a: '' }, command: 'PING' })).toBe('@a PING');
  });

  test('a tag value is escaped on the way out', () => {
    expect(formatMessage({ tags: { x: 'a b;c' }, command: 'PING' })).toBe('@x=a\\sb\\:c PING');
  });

  test('round trip', () => {
    const line = '@+draft/reply=abc;msgid=xyz :bob!u@h PRIVMSG #c :hello there';
    const m = parseMessage(line)!;
    const back = formatMessage({ tags: m.tags as Record<string, string>, command: m.command, params: m.params });

    // The prefix is the server's to write, so what comes back is the
    // line minus it.
    expect(back).toBe('@+draft/reply=abc;msgid=xyz PRIVMSG #c :hello there');
  });
});

describe('splitForLine', () => {
  test('a short message is left alone', () => {
    expect(splitForLine('hello', 20)).toEqual(['hello']);
  });

  test('pieces fit', () => {
    const text = 'word '.repeat(300).trim();
    const pieces = splitForLine(text, 50);

    expect(pieces.length).toBeGreaterThan(1);
    for (const p of pieces) expect(byteLength(p)).toBeLessThanOrEqual(512 - 50);
  });

  test('nothing is lost', () => {
    const text = 'alpha beta gamma delta '.repeat(60).trim();

    expect(splitForLine(text, 100).join(' ')).toBe(text);
  });

  test('a multi-byte character is never cut in half', () => {
    // 200 emoji, each four bytes: every piece has to decode.
    const text = '😀'.repeat(200);
    const pieces = splitForLine(text, 0, 64);

    for (const p of pieces) {
      expect(byteLength(p) % 4).toBe(0);
      expect([...p].every((c) => c === '😀')).toBe(true);
    }

    expect(pieces.join('')).toBe(text);
  });

  test('a word longer than a line is broken rather than dropped', () => {
    const text = 'x'.repeat(2000);
    const pieces = splitForLine(text, 0, 100);

    expect(pieces.join('')).toBe(text);
    for (const p of pieces) expect(p.length).toBeLessThanOrEqual(100);
  });
});

describe('byteLength', () => {
  test('counts UTF-8 bytes and not code units', () => {
    // A length checked in UTF-16 units silently allows an over-long line
    // the moment somebody types an emoji.
    expect(byteLength('abc')).toBe(3);
    expect(byteLength('é')).toBe(2);
    expect(byteLength('😀')).toBe(4);
    expect('😀'.length).toBe(2);
  });
});
