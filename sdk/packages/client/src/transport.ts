/**
 * The one thing that differs between web, mobile and desktop.
 *
 * Everything above this interface is the same code in a browser, in
 * React Native, in a Wails webview and under Bun.  Everything that is
 * not the same is here, and it is four methods.
 *
 * The WebSocket implementation below is enough for all four of those --
 * `WebSocket` is a global in every one of them, and Wails is a webview
 * so it is the browser's.  React Native's is a polyfill with two
 * differences, both handled here.  The interface exists anyway, because
 * a client that cannot be driven by a fake transport is a client whose
 * reconnection logic is never tested.
 */

/** What a transport tells the client. */
export interface TransportHandlers {
  /** The connection is up.  Registration starts now. */
  onOpen(): void;
  /** One complete line, without its CR LF. */
  onLine(line: string): void;
  /** The connection went away.  `clean` is false for anything the
   * client did not ask for, which is what decides whether to reconnect. */
  onClose(clean: boolean, reason: string): void;
  /** Something went wrong that did not close the connection. */
  onError(error: Error): void;
}

/** A way to reach a server. */
export interface Transport {
  connect(handlers: TransportHandlers): void;
  /** Send one line.  The transport adds the CR LF. */
  send(line: string): void;
  /** Close it.  `onClose` is called with `clean` set. */
  close(code?: number, reason?: string): void;
  /** Non-zero when there is an open connection. */
  readonly connected: boolean;
}

/** What `WebSocketTransport` is built with. */
export interface WebSocketOptions {
  /** `wss://irc.example.net/` -- the server's WebSocket port. */
  readonly url: string;
  /**
   * Which subprotocol to ask for.
   *
   * This server offers `text.ircv3.net` and `binary.ircv3.net` and
   * chooses in the client's order of preference.  Text is the default
   * because every runtime here gives a string to `onmessage` for a text
   * frame, while a binary frame arrives as a `Blob` in a browser, an
   * `ArrayBuffer` under Bun and either in React Native depending on
   * `binaryType` -- three shapes for the same bytes.
   *
   * Binary is the right choice only if something in the path is known to
   * mangle UTF-8, which nothing in this one does.
   */
  readonly subprotocols?: readonly string[];
  /** A factory, for a runtime whose WebSocket is not a global, and for
   * tests. */
  readonly factory?: (url: string, protocols: readonly string[]) => WebSocketLike;
}

/** The part of `WebSocket` this uses.  Named so a fake can be written
 * without pulling in DOM types. */
export interface WebSocketLike {
  send(data: string): void;
  close(code?: number, reason?: string): void;
  onopen: ((ev: unknown) => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
  onclose: ((ev: { code?: number; reason?: string; wasClean?: boolean }) => void) | null;
  onerror: ((ev: unknown) => void) | null;
  readonly readyState: number;
  binaryType?: string;
}

const OPEN = 1;

/** No IRC message is this long, so anything bigger is not one.  The
 * limit exists because a frame is delivered whole and a peer that is not
 * this server could make it any size it liked. */
const FRAME_MAX = 64 * 1024;

/** IRC over WebSocket, for every runtime this SDK targets. */
export class WebSocketTransport implements Transport {
  private socket: WebSocketLike | undefined;
  private handlers: TransportHandlers | undefined;
  private closing = false;

  constructor(private readonly options: WebSocketOptions) {}

  get connected(): boolean {
    return this.socket?.readyState === OPEN;
  }

  connect(handlers: TransportHandlers): void {
    this.handlers = handlers;
    this.closing = false;

    const protocols = this.options.subprotocols ?? ['text.ircv3.net'];
    const make =
      this.options.factory ??
      ((url: string, p: readonly string[]) =>
        new (globalThis as { WebSocket: new (u: string, p: string[]) => WebSocketLike }).WebSocket(
          url,
          [...p],
        ));

    let socket: WebSocketLike;

    try {
      socket = make(this.options.url, protocols);
    } catch (err) {
      handlers.onError(err instanceof Error ? err : new Error(String(err)));
      handlers.onClose(false, 'could not open a socket');
      return;
    }

    this.socket = socket;

    // Under Bun and in a browser a binary frame is a Blob by default,
    // and reading one is asynchronous -- which would reorder lines.  An
    // ArrayBuffer is not, and every runtime here honours this.
    if ('binaryType' in socket) socket.binaryType = 'arraybuffer';

    socket.onopen = () => handlers.onOpen();

    socket.onmessage = (ev) => {
      const text = decodeFrame(ev.data);

      if (text === undefined) return;

      this.feed(text);
    };

    socket.onclose = (ev) => {
      this.socket = undefined;
      handlers.onClose(this.closing || ev.wasClean === true, ev.reason ?? '');
    };

    socket.onerror = () => {
      // A WebSocket error event carries nothing useful by design -- the
      // specification says so, to avoid leaking why a cross-origin
      // connection failed.  Pretending otherwise would be inventing a
      // reason; `onclose` follows with what there is.
      handlers.onError(new Error('the connection failed'));
    };
  }

  send(line: string): void {
    if (!this.socket || this.socket.readyState !== OPEN) return;

    this.socket.send(`${line}\r\n`);
  }

  close(code = 1000, reason = ''): void {
    this.closing = true;

    if (!this.socket) {
      this.handlers?.onClose(true, reason);
      return;
    }

    this.socket.close(code, reason);
  }

  /** Deliver what arrived in one frame.
   *
   * **A WebSocket message boundary is a message boundary**, which is the
   * one thing IRC over WebSocket changes about reading the protocol: the
   * `text.ircv3.net` subprotocol says one IRC message per frame *with no
   * trailing CR LF*, and this server's `websocket.c` strips it before
   * framing.  A client that buffered until it saw a newline -- the right
   * thing over a TCP stream -- would therefore never deliver a single
   * line, which is exactly what this SDK did until a live server said
   * nothing back.
   *
   * Fragmentation cannot split a message either: a fragmented WebSocket
   * message is reassembled by the WebSocket layer before `onmessage`
   * fires, so there is no partial line to carry over.
   *
   * The split on newlines stays, because a server is still allowed to put
   * several messages in one frame (the spec's "one per frame" is a SHOULD)
   * and because an implementation that sends the terminator anyway is
   * common.  Either way each piece is one line here, and nothing is held.
   */
  private feed(text: string): void {
    if (text.length > FRAME_MAX) {
      this.handlers?.onError(new Error('the peer sent an oversized frame'));
      this.close(1009, 'frame too large');
      return;
    }

    for (const piece of text.split('\n')) {
      const line = piece.replace(/\r$/, '');

      if (line) this.handlers?.onLine(line);
    }
  }
}

/** Turn whatever `onmessage` gave us into text. */
function decodeFrame(data: unknown): string | undefined {
  if (typeof data === 'string') return data;

  if (data instanceof ArrayBuffer) return new TextDecoder().decode(data);

  if (ArrayBuffer.isView(data)) {
    return new TextDecoder().decode(
      new Uint8Array(data.buffer, data.byteOffset, data.byteLength),
    );
  }

  // A Blob, which only turns up if `binaryType` could not be set.  It is
  // read asynchronously, and reading it here would let a later frame
  // overtake it -- so it is dropped, loudly, rather than delivered out of
  // order.  In practice this never happens: `binaryType` is honoured
  // everywhere this SDK runs.
  return undefined;
}
