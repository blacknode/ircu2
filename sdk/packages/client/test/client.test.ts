import { describe, expect, test } from 'bun:test';

import { Client, type ClientOptions } from '../src/client.js';
import type { ClientEvent } from '../src/events.js';
import { NetworkState } from '../src/state.js';
import { FakeClock, FakeTransport, register } from './fake.js';

function make(options: Partial<Omit<ClientOptions, 'transport'>> = {}) {
  const transport = new FakeTransport();
  const clock = new FakeClock();
  const events: ClientEvent[] = [];
  const client = new Client({
    transport,
    nick: 'alice',
    setTimeout: clock.setTimeout,
    clearTimeout: clock.clearTimeout,
    ...options,
  });

  client.on((e) => events.push(e));

  return { client, transport, clock, events };
}

const of = <T extends ClientEvent['type']>(events: ClientEvent[], type: T) =>
  events.filter((e) => e.type === type) as Extract<ClientEvent, { type: T }>[];

describe('registration', () => {
  test('CAP goes first, before NICK and USER', () => {
    // Registration is what CAP END releases, so a client that sent
    // USER/NICK before negotiating would have finished registering
    // before it knew what the server could do.
    const { client, transport } = make();

    client.connect();
    transport.accept();

    expect(transport.sent[0]).toBe('CAP LS 302');
    expect(transport.sent).toContain('NICK alice');
    expect(transport.sent.some((l) => l.startsWith('USER alice 0 *'))).toBe(true);
  });

  test('a server with no capabilities still registers', () => {
    const { client, transport, events } = make();

    client.connect();
    transport.accept();
    transport.say('CAP * LS :');
    transport.say(':irc.example.net 001 alice :Welcome');

    expect(transport.sent).toContain('CAP END');
    expect(of(events, 'registered')[0]?.nick).toBe('alice');
    expect(client.connectionStatus).toBe('ready');
  });

  test('PING is answered before anything else is considered', () => {
    const { client, transport } = make();

    client.connect();
    transport.accept();
    transport.take();
    transport.say('PING :abc123');

    // Not "PONG :abc123": a parameter with no space in it is not made
    // trailing, which is the byte-for-byte compatibility the formatter
    // exists for.
    expect(transport.sent).toEqual(['PONG abc123']);
  });

  test('a password goes out as PASS, which is not authenticating', () => {
    const { client, transport } = make({ password: 'letmein' });

    client.connect();
    transport.accept();

    expect(transport.sent).toContain('PASS letmein');
  });

  test('languages are asked for when there are any', () => {
    const { client, transport } = make({ languages: ['es', 'en'] });

    client.connect();
    transport.accept();

    expect(transport.sent).toContain('LANGUAGE es en');
  });
});

describe('identity', () => {
  test('SASL runs before CAP END, and CAP END waits for it', () => {
    const { client, transport, events } = make({
      credential: { mechanism: 'PLAIN', address: 'alice@example.com', password: 'pw' },
    });

    client.connect();
    transport.accept();
    transport.take();

    transport.say('CAP * LS :sasl=PLAIN,EXTERNAL message-tags');
    transport.say('CAP * ACK :sasl message-tags');

    // Not yet: the client waits for 903, because CAP END is what lets
    // registration finish and ending here would throw the login away.
    expect(transport.sent).not.toContain('CAP END');
    expect(transport.sent).toContain('AUTHENTICATE PLAIN');

    transport.say('AUTHENTICATE +');
    transport.say(':irc.example.net 903 alice alice :SASL authentication successful');

    expect(transport.sent).toContain('CAP END');
    expect(of(events, 'identified')[0]?.account).toBe('alice');
    expect(client.state.identified).toBe(true);
  });

  test('a mechanism the server does not offer is said so, not attempted', () => {
    const { client, transport, events } = make({
      credential: { mechanism: 'EXTERNAL' },
    });

    client.connect();
    transport.accept();
    transport.say('CAP * LS :sasl=PLAIN');
    transport.say('CAP * ACK :sasl');

    expect(of(events, 'identify-failed')[0]?.reason).toContain('EXTERNAL');
    expect(transport.sent).toContain('CAP END');
  });

  test('a failed login still lets registration finish', () => {
    // Being refused is not being disconnected: the server carries on and
    // so does the client, as somebody who did not identify.
    const { client, transport, events } = make({
      credential: { mechanism: 'PLAIN', address: 'alice@example.com', password: 'wrong' },
    });

    client.connect();
    transport.accept();
    transport.say('CAP * LS :sasl=PLAIN');
    transport.say('CAP * ACK :sasl');
    transport.say('AUTHENTICATE +');
    transport.say(':irc.example.net 904 alice :SASL authentication failed');

    expect(of(events, 'identify-failed').length).toBe(1);
    expect(transport.sent).toContain('CAP END');
    expect(client.state.identified).toBe(false);
  });

  test('+r sets identified, and the account is the nickname', () => {
    // There is no account name here.  `+r` means "this person proved the
    // nickname they are wearing", so when it is set the account *is*
    // `state.nick`.
    const { client, transport, events } = make();

    client.connect();
    register(transport);
    transport.say(':irc.example.net MODE alice :+r');

    expect(client.state.identified).toBe(true);
    expect(of(events, 'identified')[0]?.account).toBe('alice');
  });

  test('-r clears it and releases the address with it', () => {
    // An identification without its address is a state the model does
    // not define, which is why the server releases them together.
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.say(':irc.example.net MODE alice :+r');
    client.state.email = 'alice@example.com';
    transport.say(':irc.example.net MODE alice :-r');

    expect(client.state.identified).toBe(false);
    expect(client.state.email).toBeUndefined();
  });

  test('+f is reported, because a frozen client looks broken otherwise', () => {
    // While frozen almost nothing may be sent: the server allows only
    // what each command declares, narrowed to talking to the service
    // that will lift it.
    const { client, transport, events } = make();

    client.connect();
    register(transport);
    transport.say(':irc.example.net MODE alice :+f');

    expect(client.state.frozen).toBe(true);
    expect(of(events, 'freeze')[0]?.frozen).toBe(true);

    transport.say(':irc.example.net MODE alice :-f');
    expect(client.state.frozen).toBe(false);
    expect(of(events, 'freeze')[1]?.frozen).toBe(false);
  });

  test('a rename we did not ask for is marked as the server’s', () => {
    // The `guest-*` rename: either we logged out, or a grace period ran
    // out on a nickname we never proved.  Both are worth showing.
    const { client, transport, events } = make();

    client.connect();
    register(transport);
    transport.say(':alice!u@h NICK :guest-a1b2c3d4');

    const renamed = of(events, 'renamed')[0]!;

    expect(renamed.to).toBe('guest-a1b2c3d4');
    expect(renamed.byServer).toBe(true);
    expect(client.state.nick).toBe('guest-a1b2c3d4');
  });

  test('a rename we asked for is not', () => {
    const { client, transport, events } = make();

    client.connect();
    register(transport);
    client.changeNick('bob');
    transport.say(':alice!u@h NICK :bob');

    expect(of(events, 'renamed')[0]?.byServer).toBe(false);
  });

  test('changing nickname leaves the account, because they are the same thing', () => {
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.say(':irc.example.net MODE alice :+r');
    expect(client.state.identified).toBe(true);

    transport.say(':alice!u@h NICK :bob');

    expect(client.state.identified).toBe(false);
  });
});

describe('messages', () => {
  test('a channel message is stored and emitted', () => {
    const { client, transport, events } = make();

    client.connect();
    register(transport);
    transport.say('@msgid=abc :bob!u@h PRIVMSG #chan :hello');

    const msg = of(events, 'message')[0]!.message;

    expect(msg.id).toBe('abc');
    expect(msg.from).toBe('bob');
    expect(msg.target).toBe('#chan');
    expect(msg.text).toBe('hello');
    expect(client.state.history('#chan').length).toBe(1);
  });

  test('a direct message is filed under the other end, whichever way it went', () => {
    // What a person thinks of as "the conversation with bob" is one
    // thing, and a store keyed by the *target* would split it in two.
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.say('@msgid=1 :bob!u@h PRIVMSG alice :from bob');
    transport.say('@msgid=2 :alice!u@h PRIVMSG bob :to bob');

    expect(client.state.history('bob').length).toBe(2);
    expect(client.state.history('alice').length).toBe(0);
  });

  test('server-time is when it was sent, not when it arrived', () => {
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.say('@time=2020-03-01T12:00:00.000Z :bob!u@h PRIVMSG #chan :old');

    expect(client.state.history('#chan')[0]!.at.toISOString()).toBe('2020-03-01T12:00:00.000Z');
  });

  test('the same msgid twice is stored once', () => {
    // A message may arrive off the wire and again from a CHATHISTORY the
    // user scrolled into; the server gives every message one
    // network-wide name precisely so this is decidable.
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.say('@msgid=abc :bob!u@h PRIVMSG #chan :hello');
    transport.say('@msgid=abc :bob!u@h PRIVMSG #chan :hello');

    expect(client.state.history('#chan').length).toBe(1);
  });

  test('CTCP ACTION is an action and not a control code on screen', () => {
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.say(':bob!u@h PRIVMSG #chan :ACTION waves');

    const msg = client.state.history('#chan')[0]!;

    expect(msg.action).toBe(true);
    expect(msg.text).toBe('waves');
  });

  test('a long message is split, and every piece fits', () => {
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.take();

    client.say('#chan', 'x'.repeat(1500));

    const lines = transport.sentOf('PRIVMSG');

    expect(lines.length).toBeGreaterThan(2);
    for (const line of lines) expect(line.length).toBeLessThanOrEqual(512);
  });

  test('with draft/multiline it is one batch instead', () => {
    const { client, transport } = make();

    client.connect();
    register(transport, ['message-tags', 'draft/multiline=max-bytes=4096,max-lines=24']);
    transport.take();

    client.say('#chan', 'line one\nline two\nline three');

    const sent = transport.sent;

    expect(sent[0]).toContain('BATCH +');
    expect(sent[0]).toContain('draft/multiline');
    expect(sent[sent.length - 1]).toContain('BATCH -');
    expect(sent.filter((l) => l.includes('PRIVMSG')).length).toBe(3);
  });

  test('without it, newlines are separate messages, because the wire has none', () => {
    const { client, transport } = make();

    client.connect();
    register(transport, ['message-tags']);
    transport.take();

    client.say('#chan', 'one\ntwo');

    expect(transport.sentOf('PRIVMSG').length).toBe(2);
    expect(transport.sent.some((l) => l.includes('BATCH'))).toBe(false);
  });

  test('markdown is only claimed when the server agreed to it', () => {
    // The server sends the plain-text equivalent to every client that did
    // not negotiate it, so marking a body as Markdown on a server that
    // never agreed would be marking it for nobody.
    const plain = make();

    plain.client.connect();
    register(plain.transport, ['message-tags']);
    plain.transport.take();
    plain.client.say('#chan', '**bold**', { markdown: true });

    expect(plain.transport.sent[0]).not.toContain('blacknode/format');

    const rich = make();

    rich.client.connect();
    register(rich.transport, ['message-tags', 'blacknode/richtext']);
    rich.transport.take();
    rich.client.say('#chan', '**bold**', { markdown: true });

    expect(rich.transport.sent[0]).toContain('+blacknode/format=markdown');
  });

  test('a reply carries the tag', () => {
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.take();

    client.say('#chan', 'agreed', { replyTo: 'abc123' });

    expect(transport.sent[0]).toContain('+draft/reply=abc123');
  });
});

describe('reactions and typing', () => {
  test('a reaction is a TAGMSG carrying both tags', () => {
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.take();

    client.react('#chan', 'abc123', '\u{1F44D}');

    expect(transport.sent[0]).toContain('+draft/reply=abc123');
    expect(transport.sent[0]).toContain('+draft/react=');
    expect(transport.sent[0]).toContain('TAGMSG #chan');
  });

  test('an incoming reaction is stored as a message of its own', () => {
    // Which is what makes removing one and reading them back in order
    // the same operations as for anything else.
    const { client, transport, events } = make();

    client.connect();
    register(transport);
    transport.say('@msgid=r1;+draft/reply=abc;+draft/react=\u{1F44D} :bob!u@h TAGMSG #chan');

    const reaction = of(events, 'reaction')[0]!;

    expect(reaction.to).toBe('abc');
    expect(reaction.message.react).toBe('\u{1F44D}');
    expect(client.state.history('#chan').length).toBe(1);
  });

  test('typing is an event and never a stored message', () => {
    // A row saying somebody was typing in March is not history.
    const { client, transport, events } = make();

    client.connect();
    register(transport);
    transport.say('@+typing=active :bob!u@h TAGMSG #chan');

    expect(of(events, 'typing')[0]?.state).toBe('active');
    expect(client.state.history('#chan').length).toBe(0);
  });
});

describe('redaction', () => {
  test('a redacted message keeps its row and loses its text', () => {
    // Kept rather than deleted so a reply pointing at it still has
    // something to point at -- the server leaves replies alone.
    const { client, transport, events } = make();

    client.connect();
    register(transport);
    transport.say('@msgid=abc :bob!u@h PRIVMSG #chan :regrettable');
    transport.say(':bob!u@h REDACT #chan abc :mistake');

    expect(client.state.history('#chan')[0]!.redacted).toBe(true);
    expect(of(events, 'redacted')[0]?.id).toBe('abc');
  });
});

describe('read markers', () => {
  test('moving one is a timestamp, not a msgid', () => {
    // "Everything up to here" is a point in time: a message arriving late
    // from a split is behind the marker if it was *sent* behind it.
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.take();

    client.markRead('#chan', new Date('2026-01-01T00:00:00.000Z'));

    expect(transport.sent[0]).toBe('MARKREAD #chan timestamp=2026-01-01T00:00:00.000Z');
  });

  test('one arriving from another of this person’s clients is taken', () => {
    // Per account, not per connection: a person reads on their phone and
    // expects their laptop to know.
    const { client, transport, events } = make();

    client.connect();
    register(transport);
    transport.say('MARKREAD #chan timestamp=2026-01-01T00:00:00.000Z');

    expect(of(events, 'read-marker')[0]?.target).toBe('#chan');
    expect(client.state.readMarkers.get('#chan')?.toISOString()).toBe(
      '2026-01-01T00:00:00.000Z',
    );
  });

  test('unread counts what is past the marker and not our own', () => {
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.say('MARKREAD #chan timestamp=2026-01-01T00:00:00.000Z');
    transport.say('@msgid=1;time=2025-12-31T00:00:00.000Z :bob!u@h PRIVMSG #chan :before');
    transport.say('@msgid=2;time=2026-01-02T00:00:00.000Z :bob!u@h PRIVMSG #chan :after');
    transport.say('@msgid=3;time=2026-01-03T00:00:00.000Z :alice!u@h PRIVMSG #chan :mine');

    expect(client.state.unread('#chan')).toBe(1);
  });
});

describe('channels', () => {
  test('joining, names and parting', () => {
    const { client, transport, events } = make();

    client.connect();
    register(transport);

    transport.say(':alice!u@h JOIN #chan');
    transport.say(':irc.example.net 353 alice = #chan :@bob +carol alice');
    transport.say(':irc.example.net 366 alice #chan :End of /NAMES list');

    const channel = client.state.channel('#chan')!;

    expect(channel.members.size).toBe(3);
    expect(channel.members.get('bob')!.op).toBe(true);
    expect(channel.members.get('carol')!.voice).toBe(true);
    expect(channel.joining).toBe(false);
    expect(of(events, 'names').length).toBe(1);

    transport.say(':alice!u@h PART #chan :bye');
    expect(client.state.channel('#chan')).toBeUndefined();
  });

  test('a nickname is folded the way this server folds it', () => {
    // RFC 1459: `[`, `]` and `\` are the capitals of `{`, `}` and `|`.
    // `toLowerCase()` would make nick[home] and nick{home} two people.
    expect(NetworkState.fold('Nick[Home]')).toBe(NetworkState.fold('nick{home}'));
    expect(NetworkState.fold('a\\b')).toBe(NetworkState.fold('a|b'));
  });

  test('somebody quitting leaves every channel', () => {
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.say(':alice!u@h JOIN #chan');
    transport.say(':irc.example.net 353 alice = #chan :bob alice');
    transport.say(':irc.example.net 366 alice #chan :End');
    transport.say(':bob!u@h QUIT :gone');

    expect(client.state.channel('#chan')!.members.has('bob')).toBe(false);
    expect(client.state.user('bob')).toBeUndefined();
  });
});

describe('batches', () => {
  test('a multiline message arrives as one message, not a burst', () => {
    const { client, transport, events } = make();

    client.connect();
    register(transport);

    transport.say('@msgid=whole :bob!u@h BATCH +x draft/multiline #chan');
    transport.say('@batch=x :bob!u@h PRIVMSG #chan :first');
    transport.say('@batch=x :bob!u@h PRIVMSG #chan :second');
    transport.say(':bob!u@h BATCH -x');

    const msgs = of(events, 'message');

    expect(msgs.length).toBe(1);
    expect(msgs[0]!.message.text).toBe('first\nsecond');
    // The whole message carries *one* msgid, and it is on the line that
    // opened the batch: the pieces carry none.
    expect(msgs[0]!.message.id).toBe('whole');
  });
});

describe('reconnection', () => {
  test('a drop schedules a retry; a requested close does not', () => {
    const { client, transport, clock, events } = make();

    client.connect();
    register(transport);

    transport.drop();

    expect(of(events, 'disconnected')[0]?.willRetry).toBe(true);
    expect(clock.scheduled).toBe(1);

    clock.run();
    expect(transport.connects).toBe(2);

    register(transport);
    client.disconnect();

    expect(of(events, 'disconnected')[1]?.willRetry).toBe(false);
    expect(clock.scheduled).toBe(0);
  });

  test('autoReconnect off means off', () => {
    const { client, transport, clock } = make({ autoReconnect: false });

    client.connect();
    register(transport);
    transport.drop();

    expect(clock.scheduled).toBe(0);
  });

  test('a reconnection keeps what was on the screen', () => {
    // Messages and read markers are not cleared: they are what the user
    // was reading a moment ago, and a reconnection is not a reason to
    // blank the screen.
    const { client, transport } = make();

    client.connect();
    register(transport);
    transport.say('@msgid=1 :bob!u@h PRIVMSG #chan :hello');
    transport.say(':alice!u@h JOIN #chan');

    transport.drop();

    expect(client.state.history('#chan').length).toBe(1);
    expect(client.state.channels.size).toBe(0);
  });
});

describe('standard replies', () => {
  test('a FAIL is machine-readable, unlike a numeric’s text', () => {
    const { client, transport, events } = make();

    client.connect();
    register(transport);
    transport.say('FAIL REDACT REDACT_FORBIDDEN #chan abc :You may not redact that');

    const reply = of(events, 'standard-reply')[0]!.reply;

    expect(reply.kind).toBe('FAIL');
    expect(reply.command).toBe('REDACT');
    expect(reply.code).toBe('REDACT_FORBIDDEN');
    expect(reply.text).toBe('You may not redact that');
  });
});

describe('listeners', () => {
  test('one that throws does not take the connection with it', () => {
    const transport = new FakeTransport();
    const client = new Client({ transport, nick: 'alice' });
    const seen: string[] = [];

    client.on(() => {
      throw new Error('a bug in the caller');
    });
    client.on((e) => seen.push(e.type));

    client.connect();
    transport.accept();

    expect(seen).toContain('connected');
  });
});
