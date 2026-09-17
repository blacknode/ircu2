/**
 * A transport that is a script rather than a socket.
 *
 * The reason `Transport` is an interface at all: a client whose
 * reconnection, registration and batch handling can only be exercised
 * against a live server is a client whose reconnection, registration and
 * batch handling are never exercised.
 */

import type { Transport, TransportHandlers } from '../src/transport.js';

export class FakeTransport implements Transport {
  /** Everything the client has sent, in order. */
  readonly sent: string[] = [];

  private handlers: TransportHandlers | undefined;
  private open = false;

  /** How many times `connect()` has been called. */
  connects = 0;

  get connected(): boolean {
    return this.open;
  }

  connect(handlers: TransportHandlers): void {
    this.handlers = handlers;
    this.connects++;
  }

  /** The socket came up. */
  accept(): void {
    this.open = true;
    this.handlers?.onOpen();
  }

  send(line: string): void {
    this.sent.push(line);
  }

  close(_code?: number, reason = ''): void {
    if (!this.open) {
      this.handlers?.onClose(true, reason);
      return;
    }

    this.open = false;
    this.handlers?.onClose(true, reason);
  }

  /** The connection dropped without being asked to. */
  drop(reason = 'connection reset'): void {
    this.open = false;
    this.handlers?.onClose(false, reason);
  }

  /** The server said this. */
  say(...lines: string[]): void {
    for (const line of lines) this.handlers?.onLine(line);
  }

  /** What was sent, and forget it. */
  take(): string[] {
    return this.sent.splice(0, this.sent.length);
  }

  /** Lines sent whose command is `command`. */
  sentOf(command: string): string[] {
    return this.sent.filter((l) => {
      const body = l.startsWith('@') ? l.slice(l.indexOf(' ') + 1) : l;

      return body.toUpperCase().startsWith(`${command.toUpperCase()} `) ||
        body.toUpperCase() === command.toUpperCase();
    });
  }
}

/** Timers a test drives by hand. */
export class FakeClock {
  private readonly pending = new Map<number, () => void>();
  private next = 1;

  readonly setTimeout = (fn: () => void): unknown => {
    const id = this.next++;

    this.pending.set(id, fn);

    return id;
  };

  readonly clearTimeout = (handle: unknown): void => {
    this.pending.delete(handle as number);
  };

  /** Run everything that is waiting. */
  run(): void {
    const due = [...this.pending.entries()];

    this.pending.clear();

    for (const [, fn] of due) fn();
  }

  get scheduled(): number {
    return this.pending.size;
  }
}

/** Bring a client all the way to registered, with the caps named. */
export function register(
  fake: FakeTransport,
  caps: string[] = ['message-tags', 'server-time', 'batch', 'labeled-response'],
): void {
  fake.accept();
  fake.say(`CAP * LS :${caps.join(' ')}`);

  // An ACK carries bare names: the value arrived with the LS, which is
  // what CAP LS 302 is for.  A server that echoed the value back would
  // be naming a capability nobody asked for.
  const bare = caps.map((c) => c.split('=')[0] as string);

  if (bare.length) fake.say(`CAP * ACK :${bare.join(' ')}`);
  fake.say(':irc.example.net 001 alice :Welcome to the UnderNode IRC Network, alice');
}
