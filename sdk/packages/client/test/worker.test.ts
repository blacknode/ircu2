import { describe, expect, test } from 'bun:test';

import { WorkerHub, mentions, type PortLike } from '../src/worker/hub.js';
import type { FromWorker, ToWorker } from '../src/worker/protocol.js';
import { FakeTransport, register } from './fake.js';

/** A tab. */
class FakePort implements PortLike {
  readonly received: FromWorker[] = [];
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  started = false;

  postMessage(value: unknown): void {
    this.received.push(value as FromWorker);
  }

  start(): void {
    this.started = true;
  }

  /** The tab said something. */
  say(msg: ToWorker): void {
    this.onmessage?.({ data: msg });
  }

  of<T extends FromWorker['kind']>(kind: T): Extract<FromWorker, { kind: T }>[] {
    return this.received.filter((m) => m.kind === kind) as Extract<FromWorker, { kind: T }>[];
  }
}

function makeHub() {
  const transport = new FakeTransport();
  const hub = new WorkerHub({ transportFactory: () => transport });

  return { hub, transport };
}

const CONNECT: ToWorker = {
  kind: 'connect',
  request: { url: 'wss://irc.example.net/', nick: 'alice' },
};

describe('WorkerHub', () => {
  test('eight tabs are one connection, not eight', () => {
    // The whole reason there is a worker: a person with eight tabs open
    // is one person, and eight connections would be eight clients
    // flooding the server and eight copies of the same history.
    const { hub, transport } = makeHub();
    const tabs = Array.from({ length: 8 }, () => new FakePort());

    for (const tab of tabs) {
      hub.attach(tab);
      tab.say(CONNECT);
    }

    expect(transport.connects).toBe(1);
    expect(hub.tabCount).toBe(8);
  });

  test('a port is started, so a tab need not remember to', () => {
    const { hub } = makeHub();
    const tab = new FakePort();

    hub.attach(tab);

    expect(tab.started).toBe(true);
  });

  test('every tab sees every event', () => {
    const { hub, transport } = makeHub();
    const a = new FakePort();
    const b = new FakePort();

    hub.attach(a);
    hub.attach(b);
    a.say(CONNECT);

    register(transport);

    for (const tab of [a, b]) {
      expect(tab.of('event').some((e) => e.event.type === 'registered')).toBe(true);
    }
  });

  test('a tab that just opened gets a snapshot, not a replay', () => {
    // Three hours of events is not what a tab opening into a
    // conversation wants; what is on the screen is.
    const { hub, transport } = makeHub();
    const first = new FakePort();

    hub.attach(first);
    first.say(CONNECT);
    register(transport);
    transport.say(':alice!u@h JOIN #chan');
    transport.say('@msgid=1 :bob!u@h PRIVMSG #chan :hello');

    const late = new FakePort();

    hub.attach(late);
    late.say({ kind: 'sync' });

    const snap = late.of('snapshot')[0]!.snapshot;

    expect(snap.nick).toBe('alice');
    expect(snap.channels.map((c) => c.name)).toContain('#chan');
    expect(late.of('event').length).toBe(0);
  });

  test('closing one tab does not close the connection', () => {
    // A person who closed one of eight tabs has not gone offline, and
    // dropping the connection would lose what the other seven show.
    const { hub, transport } = makeHub();
    const a = new FakePort();
    const b = new FakePort();

    hub.attach(a);
    hub.attach(b);
    a.say(CONNECT);
    register(transport);

    hub.detach(a);

    expect(hub.tabCount).toBe(1);
    expect(transport.connected).toBe(true);
  });

  test('a tab sends and it goes out once', () => {
    const { hub, transport } = makeHub();
    const tab = new FakePort();

    hub.attach(tab);
    tab.say(CONNECT);
    register(transport);
    transport.take();

    tab.say({ kind: 'say', target: '#chan', text: 'hello' });

    expect(transport.sentOf('PRIVMSG')).toEqual(['PRIVMSG #chan hello']);
  });
});

describe('notifications', () => {
  function setup() {
    const { hub, transport } = makeHub();
    const visible = new FakePort();
    const hidden = new FakePort();

    hub.attach(visible);
    hub.attach(hidden);
    visible.say(CONNECT);
    register(transport);

    visible.say({ kind: 'visibility', visible: true });
    hidden.say({ kind: 'visibility', visible: false });

    return { hub, transport, visible, hidden };
  }

  test('a mention notifies once, in the hidden tab', () => {
    // Eight tabs each raising the same notification is eight
    // notifications for one message.
    const { transport, visible, hidden } = setup();

    transport.say('@msgid=1 :bob!u@h PRIVMSG #chan :alice: look at this');

    expect(hidden.of('notify').length).toBe(1);
    expect(visible.of('notify').length).toBe(0);
    expect(hidden.of('notify')[0]!.mention).toBe(true);
  });

  test('a direct message always counts as one', () => {
    const { transport, hidden } = setup();

    transport.say('@msgid=1 :bob!u@h PRIVMSG alice :are you there');

    expect(hidden.of('notify').length).toBe(1);
    expect(hidden.of('notify')[0]!.mention).toBe(false);
  });

  test('a channel message that is not a mention does not', () => {
    const { transport, hidden } = setup();

    transport.say('@msgid=1 :bob!u@h PRIVMSG #chan :chatting away');

    expect(hidden.of('notify').length).toBe(0);
  });

  test('nothing is raised when every tab is on screen', () => {
    // A mention in a window the person is looking at does not need
    // announcing.
    const { hub, transport } = makeHub();
    const tab = new FakePort();

    hub.attach(tab);
    tab.say(CONNECT);
    register(transport);
    tab.say({ kind: 'visibility', visible: true });

    transport.say('@msgid=1 :bob!u@h PRIVMSG #chan :alice: hi');

    expect(tab.of('notify').length).toBe(0);
  });

  test('our own message is not news', () => {
    const { transport, hidden } = setup();

    transport.say('@msgid=1 :alice!u@h PRIVMSG #chan :alice was here');

    expect(hidden.of('notify').length).toBe(0);
  });

  test('replayed history is not news either', () => {
    const { hub, transport } = makeHub();
    const tab = new FakePort();

    hub.attach(tab);
    tab.say(CONNECT);
    register(transport, ['message-tags', 'batch', 'labeled-response', 'draft/chathistory']);
    tab.say({ kind: 'visibility', visible: false });

    // Scrolling back three hours should not ring three hours of bells.
    void hub.connection!.history('#chan', { shape: 'LATEST', limit: 50 });

    const label = transport.sent
      .map((l) => /label=([^ ;]+)/.exec(l)?.[1])
      .find((x): x is string => Boolean(x))!;

    transport.say(`@label=${label} BATCH +h chathistory #chan`);
    transport.say('@batch=h;msgid=old :bob!u@h PRIVMSG #chan :alice: from the past');
    transport.say('BATCH -h');

    expect(tab.of('notify').length).toBe(0);
  });
});

describe('mentions', () => {
  test('a whole word, and not part of one', () => {
    expect(mentions('alice: hello', 'alice')).toBe(true);
    expect(mentions('hey alice', 'alice')).toBe(true);
    expect(mentions('(alice)', 'alice')).toBe(true);
    expect(mentions('alicerules', 'alice')).toBe(false);
    expect(mentions('malice', 'alice')).toBe(false);
  });

  test('case is the server’s casemapping, not ASCII’s', () => {
    expect(mentions('ALICE: hi', 'alice')).toBe(true);
    expect(mentions('nick{home}: hi', 'Nick[Home]')).toBe(true);
  });

  test('a nickname full of regular-expression syntax still works', () => {
    // A nickname may contain [ ] \ ^ { } |, several of which are regex
    // metacharacters -- which is why the match is done by hand.
    expect(mentions('hey a[b]c, look', 'a[b]c')).toBe(true);
    expect(mentions('nothing here', 'a[b]c')).toBe(false);
  });

  test('an empty nickname matches nothing', () => {
    expect(mentions('anything at all', '')).toBe(false);
  });
});

describe('history over a port', () => {
  test('the answer goes to the tab that asked', async () => {
    const { hub, transport } = makeHub();
    const asker = new FakePort();
    const other = new FakePort();

    hub.attach(asker);
    hub.attach(other);
    asker.say(CONNECT);
    register(transport, ['message-tags', 'batch', 'labeled-response', 'draft/chathistory']);

    asker.say({
      kind: 'history',
      id: 7,
      target: '#chan',
      selector: { shape: 'LATEST', limit: 10 },
    });

    const label = transport.sent
      .map((l) => /label=([^ ;]+)/.exec(l)?.[1])
      .find((x): x is string => Boolean(x))!;

    transport.say(`@label=${label} BATCH +h chathistory #chan`);
    transport.say('@batch=h;msgid=1 :bob!u@h PRIVMSG #chan :one');
    transport.say('BATCH -h');

    // Two ticks: the label resolves, `history()` awaits it, and the hub
    // posts on the turn after that.
    await Promise.resolve();
    await Promise.resolve();

    expect(asker.of('history').length).toBe(1);
    expect(asker.of('history')[0]!.id).toBe(7);
    expect(other.of('history').length).toBe(0);
  });

  test('a tab that closed mid-answer is not answered, and nobody else is', async () => {
    // Giving it to somebody else would be answering a question they did
    // not ask.
    const { hub, transport } = makeHub();
    const asker = new FakePort();
    const other = new FakePort();

    hub.attach(asker);
    hub.attach(other);
    asker.say(CONNECT);
    register(transport, ['message-tags', 'batch', 'labeled-response', 'draft/chathistory']);

    asker.say({
      kind: 'history',
      id: 9,
      target: '#chan',
      selector: { shape: 'LATEST', limit: 10 },
    });

    hub.detach(asker);

    const label = transport.sent
      .map((l) => /label=([^ ;]+)/.exec(l)?.[1])
      .find((x): x is string => Boolean(x))!;

    transport.say(`@label=${label} BATCH +h chathistory #chan`);
    transport.say('@batch=h;msgid=1 :bob!u@h PRIVMSG #chan :one');
    transport.say('BATCH -h');

    await Promise.resolve();
    await Promise.resolve();

    expect(asker.of('history').length).toBe(0);
    expect(other.of('history').length).toBe(0);
  });
});
