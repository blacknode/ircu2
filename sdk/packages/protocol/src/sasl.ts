/**
 * SASL, the client's half.
 *
 * The mirror of `ircd/sasl.c`: the *shape* of an authentication and
 * never the answer.  It turns a credential into AUTHENTICATE lines and
 * the server's replies into an outcome, and it holds no opinion about
 * whether the credential is any good -- that is the identity module's,
 * on the other end.
 *
 * On this server an **account is a nickname** (proposal 007).  So the
 * authcid is the *address* you registered with, the authzid is *which of
 * that address's nicknames* you are claiming, and there is no third
 * concept anywhere.  An SDK that modelled "account" as something separate
 * from the nickname would be modelling a server that does not exist.
 */

/** A credential to authenticate with. */
export type Credential =
  | {
      readonly mechanism: 'PLAIN';
      /** The address the account is registered under. */
      readonly address: string;
      readonly password: string;
      /** Which of that address's nicknames to log in as.
       *
       * Omit it and the server picks, which is what a single-account
       * address wants.  Name it when the address has more than one.
       */
      readonly account?: string;
    }
  | {
      /** The credential is the certificate the connection already has;
       * matching it *is* the proof, so there is nothing to send. */
      readonly mechanism: 'EXTERNAL';
      readonly account?: string;
    };

/** How an exchange ended. */
export type SaslOutcome =
  | { readonly ok: true; readonly account: string }
  | { readonly ok: false; readonly reason: string; readonly aborted: boolean };

/** What the state machine wants done next. */
export interface SaslStep {
  readonly send: readonly string[];
  readonly outcome?: SaslOutcome;
}

const EMPTY: SaslStep = { send: [] };

/** The server chunks its side at 400 characters, and expects the same. */
const CHUNK = 400;

/** Encode bytes as base64, in every runtime this SDK targets.
 *
 * Neither `btoa` (browser, Latin-1 only) nor `Buffer` (node/bun only) is
 * everywhere, and React Native has one of them depending on the version.
 * Doing it here costs twenty lines and removes a whole class of "works
 * on web, not on the phone".
 */
export function toBase64(bytes: Uint8Array): string {
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  let out = '';

  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i] as number;
    const b1 = bytes[i + 1];
    const b2 = bytes[i + 2];

    out += alphabet[b0 >> 2];
    out += alphabet[((b0 & 0x03) << 4) | ((b1 ?? 0) >> 4)];
    out += b1 === undefined ? '=' : alphabet[((b1 & 0x0f) << 2) | ((b2 ?? 0) >> 6)];
    out += b2 === undefined ? '=' : alphabet[b2 & 0x3f];
  }

  return out;
}

/** And back.  Strict, because it decodes what a server sent. */
export function fromBase64(text: string): Uint8Array {
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const clean = text.replace(/=+$/, '');
  const out = new Uint8Array(Math.floor((clean.length * 6) / 8));
  let bits = 0;
  let acc = 0;
  let n = 0;

  for (const ch of clean) {
    const v = alphabet.indexOf(ch);

    if (v < 0) throw new Error(`not base64: ${JSON.stringify(ch)}`);

    acc = (acc << 6) | v;
    bits += 6;

    if (bits >= 8) {
      bits -= 8;
      out[n++] = (acc >> bits) & 0xff;
    }
  }

  return out.subarray(0, n);
}

/** Build the PLAIN payload: authzid NUL authcid NUL password. */
function plainPayload(cred: Extract<Credential, { mechanism: 'PLAIN' }>): Uint8Array {
  const enc = new TextEncoder();
  const authzid = enc.encode(cred.account ?? '');
  const authcid = enc.encode(cred.address);
  const secret = enc.encode(cred.password);
  const out = new Uint8Array(authzid.length + 1 + authcid.length + 1 + secret.length);
  let at = 0;

  out.set(authzid, at);
  at += authzid.length;
  out[at++] = 0;
  out.set(authcid, at);
  at += authcid.length;
  out[at++] = 0;
  out.set(secret, at);

  return out;
}

/** One authentication, from the first AUTHENTICATE to the numeric. */
export class SaslSession {
  private payload: string;
  private sent = false;
  private finished = false;

  constructor(private readonly cred: Credential) {
    this.payload =
      cred.mechanism === 'EXTERNAL'
        ? // EXTERNAL's payload is the authzid, or nothing at all when the
          // certificate speaks for itself.
          cred.account
          ? toBase64(new TextEncoder().encode(cred.account))
          : '+'
        : toBase64(plainPayload(cred));
  }

  /** Begin: name the mechanism. */
  start(): SaslStep {
    return { send: [`AUTHENTICATE ${this.cred.mechanism}`] };
  }

  /** Abandon it.  The server answers 906. */
  abort(): SaslStep {
    if (this.finished) return EMPTY;

    this.finished = true;

    return { send: ['AUTHENTICATE *'] };
  }

  /**
   * Feed one line.
   *
   * @param command The command or numeric, uppercased.
   * @param params Its parameters.
   */
  handle(command: string, params: readonly string[]): SaslStep {
    if (this.finished) return EMPTY;

    switch (command) {
      case 'AUTHENTICATE':
        // "+" is the server saying "go ahead".  Anything else is a
        // challenge, and neither mechanism here has one -- so an SDK
        // that pretended to answer would be inventing a protocol.
        if (params[0] !== '+') {
          this.finished = true;
          return {
            send: ['AUTHENTICATE *'],
            outcome: {
              ok: false,
              aborted: true,
              reason: 'the server sent a challenge this mechanism does not have',
            },
          };
        }

        if (this.sent) return EMPTY;

        this.sent = true;

        return { send: chunkPayload(this.payload) };

      case '900':
        // RPL_LOGGEDIN: <nick> <nick!user@host> <account> :...
        // The account is a nickname on this server, and the third
        // parameter is where it is.  It is not the end of the exchange
        // -- 903 is -- so there is nothing to report yet.
        return EMPTY;

      case '903':
        this.finished = true;
        return {
          send: [],
          outcome: { ok: true, account: params[1] ?? '' },
        };

      case '902': // ERR_NICKLOCKED
      case '904': // ERR_SASLFAIL
      case '905': // ERR_SASLTOOLONG
      case '906': // ERR_SASLABORTED
      case '907': // ERR_SASLALREADY
      case '908': // RPL_SASLMECHS
        this.finished = true;
        return {
          send: [],
          outcome: {
            ok: false,
            aborted: command === '906',
            reason: params[params.length - 1] ?? `SASL failed (${command})`,
          },
        };

      default:
        return EMPTY;
    }
  }
}

/** Split a payload into AUTHENTICATE lines the way the spec wants.
 *
 * Every piece is exactly 400 characters except the last, and a payload
 * whose length is an exact multiple of 400 is followed by a bare `+`.
 * Without that trailing line the server cannot tell "finished" from
 * "more coming", which is the one detail this function exists for.
 */
export function chunkPayload(payload: string): string[] {
  if (payload === '+') return ['AUTHENTICATE +'];

  const lines: string[] = [];

  for (let i = 0; i < payload.length; i += CHUNK) {
    lines.push(`AUTHENTICATE ${payload.slice(i, i + CHUNK)}`);
  }

  if (payload.length % CHUNK === 0) lines.push('AUTHENTICATE +');

  return lines;
}
