# @blacknode/sdk

The client half of this server's protocol, in TypeScript: message tags,
batches, labeled responses, SASL, multiline, history, read markers,
redaction and rich text, behind an API that does not ask you to have read
twelve specifications.

It lives in the same repository as the server on purpose. The wire is one
thing, and a change to `ircd/msg_tag.c` and a change to
`packages/protocol` are the same change.

**The long form is [`doc/readme.sdk`](../doc/readme.sdk).** This is how to
run it.

## Packages

| | |
|---|---|
| `@blacknode/irc-protocol` | The wire and nothing else. Pure: no sockets, no timers, no globals, no dependencies. |
| `@blacknode/irc-client` | A connection, the state it carries, and the conversation on top. |

Targets: **web** (Next.js on Bun), **mobile** (React Native), **desktop**
(Wails). The same code on all three — what differs is the transport, and
that is an interface with four methods.

## Use

```ts
import { Client, WebSocketTransport } from '@blacknode/irc-client';

const client = new Client({
  transport: new WebSocketTransport({ url: 'wss://irc.example.net/' }),
  nick: 'maria',
  credential: { mechanism: 'PLAIN', address: 'maria@example.com', password: '...' },
});

client.on((event) => {
  if (event.type === 'message') console.log(event.message.text);
});

client.connect();
```

In a browser the connection belongs in a SharedWorker, so that ten tabs
are one connection and one notification:

```ts
// app/irc.worker.ts
import '@blacknode/irc-client/worker/shared-worker';
```

## Develop

```sh
bun install
bun test           # everything; no server, no network
bun run typecheck  # strict, and then some
```

Against a real server — not part of `bun test`, because it needs an ircd
with `Port { port = 7799; websocket = yes; };`:

```sh
bun packages/client/test/live.ts ws://127.0.0.1:7799/
```

## Licence

GPL-2.0, the same as the server.
