/**
 * The SDK against a real server.
 *
 * Not part of `bun test`: it needs an ircd with a WebSocket port, which
 * the unit tests deliberately do not.  Run it by hand when the wire
 * changes, which is the only time a fake transport cannot tell you
 * anything.
 *
 *   bun packages/client/test/live.ts ws://127.0.0.1:7799/
 *
 * It exits non-zero on the first thing that is not what it should be, so
 * it is usable from a script.
 */

import { Client, WebSocketTransport } from '../src/index.js';
import type { ClientEvent } from '../src/events.js';

const url = process.argv[2] ?? 'ws://127.0.0.1:7799/';
const results: { name: string; ok: boolean; detail?: string }[] = [];

function check(name: string, ok: boolean, detail = ''): void {
  results.push({ name, ok, ...(detail ? { detail } : {}) });
  console.log(`${ok ? '  ok  ' : ' FAIL '} ${name}${detail ? ` -- ${detail}` : ''}`);
}

/** Connect one client and wait until it is registered. */
function connect(nick: string): Promise<{ client: Client; events: ClientEvent[] }> {
  const events: ClientEvent[] = [];
  const client = new Client({
    transport: new WebSocketTransport({ url }),
    nick,
    realname: 'SDK live test',
    autoReconnect: false,
  });

  client.on((e) => events.push(e));

  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${nick} never registered`)), 15_000);

    client.on((e) => {
      if (e.type === 'registered') {
        clearTimeout(timer as never);
        resolve({ client, events });
      }
      if (e.type === 'disconnected' && e.reason !== 'requested') {
        clearTimeout(timer as never);
        reject(new Error(`${nick}: ${e.reason} ${e.detail}`));
      }
    });

    client.connect();
  });
}

/** Wait for an event, or give up. */
function waitFor<T extends ClientEvent['type']>(
  client: Client,
  type: T,
  match: (e: Extract<ClientEvent, { type: T }>) => boolean = () => true,
  ms = 8000,
): Promise<Extract<ClientEvent, { type: T }>> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      off();
      reject(new Error(`timed out waiting for ${type}`));
    }, ms);

    const off = client.on((e) => {
      if (e.type !== type) return;
      if (!match(e as Extract<ClientEvent, { type: T }>)) return;

      clearTimeout(timer as never);
      off();
      resolve(e as Extract<ClientEvent, { type: T }>);
    });
  });
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function main(): Promise<void> {
  console.log(`live test against ${url}\n`);

  const tag = Math.random().toString(36).slice(2, 8);
  const alice = await connect(`a-${tag}`);
  const bob = await connect(`b-${tag}`);

  check('two clients registered over WebSocket', true);
  check(
    'the server advertised capabilities we use',
    alice.client.caps.active.size > 0,
    [...alice.client.caps.active].join(' '),
  );

  const channel = `#sdk-${tag}`;

  alice.client.join(channel);
  bob.client.join(channel);
  await waitFor(alice.client, 'names', (e) => e.channel.name.toLowerCase() === channel);
  await sleep(500);

  check(
    'both are in the channel',
    (alice.client.state.channel(channel)?.members.size ?? 0) >= 2,
    `members: ${[...(alice.client.state.channel(channel)?.members.keys() ?? [])].join(',')}`,
  );

  // --- a plain message, and the msgid that everything else hangs on ---
  const heard = waitFor(bob.client, 'message', (e) => e.message.text === 'hello from the sdk');

  alice.client.say(channel, 'hello from the sdk');

  const got = await heard;

  check('a message arrives', got.message.text === 'hello from the sdk');
  check('it carries a msgid', Boolean(got.message.id), got.message.id ?? '(none)');
  check(
    'server-time says when it was sent',
    got.message.at.getTime() > Date.now() - 60_000,
    got.message.at.toISOString(),
  );

  const firstId = got.message.id as string;

  // --- a reply ---
  const replied = waitFor(bob.client, 'message', (e) => e.message.replyTo === firstId);

  alice.client.say(channel, 'and a reply', { replyTo: firstId });
  const reply = await replied;

  check('a reply names what it follows', reply.message.replyTo === firstId);

  // --- a reaction ---
  const reacted = waitFor(bob.client, 'reaction', (e) => e.to === firstId);

  alice.client.react(channel, firstId, '\u{1F44D}');
  const reaction = await reacted;

  check('a reaction arrives as a reaction', reaction.message.react === '\u{1F44D}');

  // --- typing, which is an event and never a message ---
  const typed = waitFor(bob.client, 'typing');

  alice.client.typing(channel, 'active');
  await typed;

  check('typing is an event', true);

  const before = bob.client.state.history(channel).length;

  await sleep(300);
  check('typing is never stored', bob.client.state.history(channel).length === before);

  // --- a long message ---
  const long = 'long '.repeat(300).trim();
  const longHeard = waitFor(bob.client, 'message', (e) => e.message.text.startsWith('long long'), 12_000);

  alice.client.say(channel, long);
  const longGot = await longHeard;

  // Byte for byte, not "most of it": a long line goes out as several
  // wire lines and comes back joined, and a client that checked only the
  // beginning would not notice the tail being dropped.
  check(
    'a long message survives the wire whole',
    longGot.message.text === long,
    `${longGot.message.text.length} of ${long.length} characters`,
  );

  // --- a direct message, filed under the other end ---
  const dm = waitFor(bob.client, 'message', (e) => e.message.text === 'just between us');

  alice.client.say(bob.client.state.nick, 'just between us');
  await dm;

  check(
    'a direct message is filed under the other end',
    bob.client.state.history(alice.client.state.nick).length === 1,
  );

  // --- what the server told us about itself ---
  check(
    'ISUPPORT was read',
    alice.client.state.isupport.has('NICKLEN'),
    `NICKLEN=${alice.client.state.isupport.get('NICKLEN')}`,
  );

  alice.client.disconnect();
  bob.client.disconnect();

  await sleep(300);

  const failed = results.filter((r) => !r.ok);

  console.log(`\n${results.length - failed.length}/${results.length} ok`);

  if (failed.length) process.exit(1);
}

main().catch((err) => {
  console.error(`\nlive test failed: ${err instanceof Error ? err.message : String(err)}`);
  process.exit(1);
});
