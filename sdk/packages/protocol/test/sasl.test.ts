import { describe, expect, test } from 'bun:test';

import { SaslSession, chunkPayload, fromBase64, toBase64 } from '../src/sasl.js';

/** Drive a session with a list of lines and collect everything sent. */
function run(s: SaslSession, lines: [string, string[]][]) {
  const sent: string[] = [...s.start().send];
  let outcome;

  for (const [command, params] of lines) {
    const step = s.handle(command, params);

    sent.push(...step.send);
    if (step.outcome) outcome = step.outcome;
  }

  return { sent, outcome };
}

describe('base64', () => {
  test('RFC 4648 vectors, both ways', () => {
    const cases: [string, string][] = [
      ['', ''],
      ['f', 'Zg=='],
      ['fo', 'Zm8='],
      ['foo', 'Zm9v'],
      ['foob', 'Zm9vYg=='],
      ['fooba', 'Zm9vYmE='],
      ['foobar', 'Zm9vYmFy'],
    ];

    for (const [plain, encoded] of cases) {
      const bytes = new TextEncoder().encode(plain);

      expect(toBase64(bytes)).toBe(encoded);
      expect(new TextDecoder().decode(fromBase64(encoded))).toBe(plain);
    }
  });

  test('NUL bytes survive, which is the whole of PLAIN', () => {
    const bytes = new Uint8Array([0x61, 0x00, 0x62, 0x00, 0x63]);

    expect([...fromBase64(toBase64(bytes))]).toEqual([...bytes]);
  });

  test('non-ASCII survives, which btoa() would not manage', () => {
    // btoa() is Latin-1 only and Buffer is not in a browser; this is why
    // the SDK carries its own twenty lines.
    const text = 'mañana 😀';
    const bytes = new TextEncoder().encode(text);

    expect(new TextDecoder().decode(fromBase64(toBase64(bytes)))).toBe(text);
  });

  test('rubbish is refused rather than decoded into rubbish', () => {
    expect(() => fromBase64('not base64!')).toThrow();
  });
});

describe('chunkPayload', () => {
  test('a short payload is one line', () => {
    expect(chunkPayload('abc')).toEqual(['AUTHENTICATE abc']);
  });

  test('every piece but the last is exactly 400', () => {
    const lines = chunkPayload('x'.repeat(950));

    expect(lines.length).toBe(3);
    expect(lines[0]!.length).toBe('AUTHENTICATE '.length + 400);
    expect(lines[1]!.length).toBe('AUTHENTICATE '.length + 400);
    expect(lines[2]!.length).toBe('AUTHENTICATE '.length + 150);
  });

  test('an exact multiple of 400 gets a trailing +', () => {
    // Without it the server cannot tell "finished" from "more coming",
    // which is the one detail this function exists for.
    const lines = chunkPayload('x'.repeat(800));

    expect(lines.length).toBe(3);
    expect(lines[2]).toBe('AUTHENTICATE +');
  });

  test('an empty payload is the bare plus', () => {
    expect(chunkPayload('+')).toEqual(['AUTHENTICATE +']);
  });
});

describe('SaslSession, PLAIN', () => {
  test('names the mechanism, then sends the credential', () => {
    const s = new SaslSession({
      mechanism: 'PLAIN',
      address: 'maria@example.com',
      password: 'hunter2',
    });

    const { sent, outcome } = run(s, [
      ['AUTHENTICATE', ['+']],
      ['900', ['maria', 'maria!u@h', 'maria', 'You are now logged in']],
      ['903', ['maria', 'maria', 'SASL authentication successful']],
    ]);

    expect(sent[0]).toBe('AUTHENTICATE PLAIN');
    expect(sent.length).toBe(2);
    expect(outcome).toEqual({ ok: true, account: 'maria' });
  });

  test('the payload is authzid NUL authcid NUL password', () => {
    const s = new SaslSession({
      mechanism: 'PLAIN',
      address: 'maria@example.com',
      password: 'hunter2',
      account: 'maria',
    });

    const { sent } = run(s, [['AUTHENTICATE', ['+']]]);
    const payload = sent[1]!.slice('AUTHENTICATE '.length);
    const decoded = new TextDecoder().decode(fromBase64(payload));

    // An account *is* a nickname here: the authcid is the address, the
    // authzid is which of that address's nicknames is being claimed.
    expect(decoded).toBe('maria\0maria@example.com\0hunter2');
  });

  test('an account left out means "you pick", which is what one account wants', () => {
    const s = new SaslSession({
      mechanism: 'PLAIN',
      address: 'maria@example.com',
      password: 'hunter2',
    });

    const { sent } = run(s, [['AUTHENTICATE', ['+']]]);
    const decoded = new TextDecoder().decode(fromBase64(sent[1]!.slice('AUTHENTICATE '.length)));

    expect(decoded).toBe('\0maria@example.com\0hunter2');
  });

  test('a long password is chunked', () => {
    const s = new SaslSession({
      mechanism: 'PLAIN',
      address: 'maria@example.com',
      password: 'x'.repeat(900),
    });

    const { sent } = run(s, [['AUTHENTICATE', ['+']]]);

    expect(sent.length).toBeGreaterThan(2);
    for (const line of sent.slice(1)) expect(line.length).toBeLessThanOrEqual(413);
  });

  test('a failure carries the server’s reason', () => {
    const s = new SaslSession({
      mechanism: 'PLAIN',
      address: 'maria@example.com',
      password: 'wrong',
    });

    const { outcome } = run(s, [
      ['AUTHENTICATE', ['+']],
      ['904', ['*', 'SASL authentication failed']],
    ]);

    expect(outcome).toEqual({
      ok: false,
      aborted: false,
      reason: 'SASL authentication failed',
    });
  });

  test('nothing is sent after it has finished', () => {
    const s = new SaslSession({ mechanism: 'PLAIN', address: 'a@b', password: 'c' });

    run(s, [
      ['AUTHENTICATE', ['+']],
      ['903', ['maria', 'maria', 'ok']],
    ]);

    expect(s.handle('AUTHENTICATE', ['+']).send).toEqual([]);
    expect(s.abort().send).toEqual([]);
  });

  test('a challenge is refused rather than answered with a guess', () => {
    // Neither mechanism here has a challenge, so a server sending one is
    // speaking something else; inventing a reply would be inventing a
    // protocol.
    const s = new SaslSession({ mechanism: 'PLAIN', address: 'a@b', password: 'c' });
    const { sent, outcome } = run(s, [['AUTHENTICATE', ['c29tZS1jaGFsbGVuZ2U=']]]);

    expect(sent).toContain('AUTHENTICATE *');
    expect(outcome!.ok).toBe(false);
  });
});

describe('SaslSession, EXTERNAL', () => {
  test('the certificate is the credential, so there is nothing to send', () => {
    const s = new SaslSession({ mechanism: 'EXTERNAL' });
    const { sent, outcome } = run(s, [
      ['AUTHENTICATE', ['+']],
      ['903', ['maria', 'maria', 'ok']],
    ]);

    expect(sent).toEqual(['AUTHENTICATE EXTERNAL', 'AUTHENTICATE +']);
    expect(outcome).toEqual({ ok: true, account: 'maria' });
  });

  test('an account names which nickname of that certificate', () => {
    const s = new SaslSession({ mechanism: 'EXTERNAL', account: 'maria' });
    const { sent } = run(s, [['AUTHENTICATE', ['+']]]);

    expect(new TextDecoder().decode(fromBase64(sent[1]!.slice('AUTHENTICATE '.length)))).toBe(
      'maria',
    );
  });
});

describe('abort', () => {
  test('sends the star, once', () => {
    const s = new SaslSession({ mechanism: 'PLAIN', address: 'a@b', password: 'c' });

    expect(s.abort().send).toEqual(['AUTHENTICATE *']);
    expect(s.abort().send).toEqual([]);
  });

  test('906 comes back marked as an abort', () => {
    const s = new SaslSession({ mechanism: 'PLAIN', address: 'a@b', password: 'c' });
    const { outcome } = run(s, [['906', ['*', 'SASL authentication aborted']]]);

    expect(outcome).toEqual({
      ok: false,
      aborted: true,
      reason: 'SASL authentication aborted',
    });
  });
});
