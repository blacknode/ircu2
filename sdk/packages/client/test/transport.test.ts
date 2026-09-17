import { describe, expect, test } from 'bun:test';

import { WebSocketTransport, type WebSocketLike } from '../src/transport.js';

/** A socket that is a script.
 *
 * Small on purpose: what is being tested is the twenty lines between a
 * frame arriving and a line being delivered, and those twenty lines were
 * wrong for a week because nothing exercised them without a server.
 */
class FakeSocket implements WebSocketLike {
  readonly sent: string[] = [];
  readonly closed: { code?: number; reason?: string }[] = [];
  binaryType = 'blob';
  readyState = 0;

  onopen: ((ev: unknown) => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  onclose: ((ev: { code?: number; reason?: string; wasClean?: boolean }) => void) | null = null;
  onerror: ((ev: unknown) => void) | null = null;

  send(data: string): void {
    this.sent.push(data);
  }

  close(code?: number, reason?: string): void {
    this.closed.push({ ...(code === undefined ? {} : { code }), ...(reason === undefined ? {} : { reason }) });
    this.readyState = 3;
    this.onclose?.({ ...(code === undefined ? {} : { code }), reason: reason ?? '', wasClean: true });
  }

  /** The handshake finished. */
  open(): void {
    this.readyState = 1;
    this.onopen?.({});
  }

  /** One frame arrived. */
  frame(data: unknown): void {
    this.onmessage?.({ data });
  }
}

function setup() {
  const socket = new FakeSocket();
  const lines: string[] = [];
  const errors: string[] = [];
  const closes: { clean: boolean; reason: string }[] = [];
  let opened = 0;

  const transport = new WebSocketTransport({
    url: 'ws://irc.example.net/',
    factory: () => socket,
  });

  transport.connect({
    onOpen: () => {
      opened++;
    },
    onLine: (l) => lines.push(l),
    onClose: (clean, reason) => closes.push({ clean, reason }),
    onError: (e) => errors.push(e.message),
  });

  return { socket, transport, lines, errors, closes, opened: () => opened };
}

describe('frames and lines', () => {
  test('a frame with no terminator is a whole message', () => {
    // The `text.ircv3.net` subprotocol sends one IRC message per frame
    // and strips the CR LF, so a transport that waits for a newline waits
    // for ever -- and looks exactly like a server that never answered.
    const { socket, lines } = setup();

    socket.open();
    socket.frame(':irc.example.net CAP * LS :message-tags batch');

    expect(lines).toEqual([':irc.example.net CAP * LS :message-tags batch']);
  });

  test('a terminator is accepted too', () => {
    const { socket, lines } = setup();

    socket.open();
    socket.frame('PING :1234\r\n');

    expect(lines).toEqual(['PING :1234']);
  });

  test('several messages in one frame are several lines', () => {
    const { socket, lines } = setup();

    socket.open();
    socket.frame(':a 001 x :hi\r\n:a 002 x :there\r\n');

    expect(lines).toEqual([':a 001 x :hi', ':a 002 x :there']);
  });

  test('an empty frame delivers nothing', () => {
    const { socket, lines } = setup();

    socket.open();
    socket.frame('');
    socket.frame('\r\n');

    expect(lines).toEqual([]);
  });

  test('bytes are decoded, whatever shape they arrive in', () => {
    const { socket, lines } = setup();
    const bytes = new TextEncoder().encode('PRIVMSG #chan :ok');

    socket.open();
    socket.frame(bytes.buffer);
    socket.frame(bytes);

    expect(lines).toEqual(['PRIVMSG #chan :ok', 'PRIVMSG #chan :ok']);
  });

  test('UTF-8 survives being split across nothing at all', () => {
    const { socket, lines } = setup();

    socket.open();
    socket.frame(new TextEncoder().encode('PRIVMSG #chan :mañana \u{1F44D}'));

    expect(lines).toEqual(['PRIVMSG #chan :mañana \u{1F44D}']);
  });

  test('an oversized frame is refused rather than kept', () => {
    const { socket, lines, errors } = setup();

    socket.open();
    socket.frame('x'.repeat(64 * 1024 + 1));

    expect(lines).toEqual([]);
    expect(errors.length).toBe(1);
    expect(socket.closed.length).toBe(1);
  });
});

describe('the socket', () => {
  test('binary frames are asked for as ArrayBuffers', () => {
    // A Blob is read asynchronously, which would let a later frame
    // overtake an earlier one.
    const { socket } = setup();

    expect(socket.binaryType).toBe('arraybuffer');
  });

  test('a line is sent with the terminator the server parses', () => {
    const { socket, transport } = setup();

    socket.open();
    transport.send('NICK alice');

    expect(socket.sent).toEqual(['NICK alice\r\n']);
  });

  test('nothing is sent before the socket is open', () => {
    const { socket, transport } = setup();

    transport.send('NICK alice');

    expect(socket.sent).toEqual([]);
  });

  test('a close the client asked for is clean', () => {
    const { socket, transport, closes } = setup();

    socket.open();
    transport.close(1000, 'bye');

    expect(closes).toEqual([{ clean: true, reason: 'bye' }]);
  });

  test('a close nobody asked for is not', () => {
    const { socket, closes } = setup();

    socket.open();
    socket.readyState = 3;
    socket.onclose?.({ code: 1006, reason: 'connection reset', wasClean: false });

    expect(closes).toEqual([{ clean: false, reason: 'connection reset' }]);
  });

  test('a socket that could not be made is a close, not a throw', () => {
    const closes: { clean: boolean; reason: string }[] = [];
    const errors: string[] = [];
    const transport = new WebSocketTransport({
      url: 'ws://irc.example.net/',
      factory: () => {
        throw new Error('name not resolved');
      },
    });

    transport.connect({
      onOpen: () => {},
      onLine: () => {},
      onClose: (clean, reason) => closes.push({ clean, reason }),
      onError: (e) => errors.push(e.message),
    });

    expect(errors).toEqual(['name not resolved']);
    expect(closes.length).toBe(1);
    expect(closes[0]!.clean).toBe(false);
  });
});
