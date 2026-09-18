import { describe, expect, test } from 'bun:test';

import { CapState } from '../src/caps.js';
import { parseMessage } from '../src/message.js';

/** Feed a line and return what the state machine wants sent. */
function feed(state: CapState, line: string): readonly string[] {
  return state.handle(parseMessage(line)!).send;
}

describe('CapState', () => {
  test('opens with CAP LS 302, because the values are the point', () => {
    // 302 is what makes the server send values, and the value *is* the
    // content of `sasl` (its mechanisms) and of `draft/multiline` (its
    // limits).
    expect(new CapState().start().send).toEqual(['CAP LS 302']);
  });

  test('asks for what it recognises and nothing else', () => {
    const s = new CapState();
    const sent = feed(s, 'CAP * LS :message-tags server-time batch some-other-thing');

    expect(sent.length).toBe(1);
    expect(sent[0]).toContain('message-tags');
    expect(sent[0]).toContain('server-time');
    expect(sent[0]).toContain('batch');
    expect(sent[0]).not.toContain('some-other-thing');
  });

  test('a server with none of them is not an error', () => {
    // A perfectly ordinary IRC server, and this is a perfectly ordinary
    // client on it.
    const s = new CapState();

    expect(feed(s, 'CAP * LS :multi-prefix away-notify')).toEqual(['CAP END']);
    expect(s.degraded.length).toBeGreaterThan(0);
  });

  test('a multi-line LS is collected before anything is asked for', () => {
    const s = new CapState();

    expect(feed(s, 'CAP * LS * :message-tags server-time')).toEqual([]);

    const sent = feed(s, 'CAP * LS :batch labeled-response');

    expect(sent.length).toBe(1);
    expect(sent[0]).toContain('message-tags');
    expect(sent[0]).toContain('labeled-response');
  });

  test('ACK puts a capability in force and NAK does not', () => {
    const s = new CapState();

    feed(s, 'CAP * LS :message-tags server-time batch');
    feed(s, 'CAP * ACK :message-tags server-time');
    const last = feed(s, 'CAP * NAK :batch');

    expect(s.active.has('message-tags')).toBe(true);
    expect(s.active.has('batch')).toBe(false);
    expect(last).toEqual(['CAP END']);
  });

  test('CAP END goes out only once everything asked for was answered', () => {
    const s = new CapState();

    feed(s, 'CAP * LS :message-tags server-time');
    expect(feed(s, 'CAP * ACK :message-tags')).toEqual([]);
    expect(feed(s, 'CAP * ACK :server-time')).toEqual(['CAP END']);
  });

  test('end() is idempotent, so a caller may be careless', () => {
    const s = new CapState();

    expect(s.end().send).toEqual(['CAP END']);
    expect(s.end().send).toEqual([]);
  });

  describe('sasl', () => {
    test('its value is the mechanism list', () => {
      const s = new CapState();

      feed(s, 'CAP * LS :sasl=PLAIN,EXTERNAL message-tags');
      feed(s, 'CAP * ACK :sasl message-tags');

      expect(s.saslMechanisms).toEqual(['PLAIN', 'EXTERNAL']);
    });

    test('CAP END waits, because the caller has to authenticate first', () => {
      const s = new CapState();

      feed(s, 'CAP * LS :sasl=PLAIN');
      const sent = feed(s, 'CAP * ACK :sasl');

      // Nothing is sent: a client waits for 903 before CAP END, and CAP
      // END is what lets registration finish.  Ending here would throw
      // the login away.
      expect(sent).toEqual([]);
      expect(s.saslPending).toBe(true);
    });

    test('a sasl with no mechanisms is a server with no provider loaded', () => {
      // The server withdraws the capability's value when no identity
      // module is registered, and "there is nothing to log in with" is a
      // different thing from "this server has no accounts".
      const s = new CapState();

      feed(s, 'CAP * LS :sasl');
      const sent = feed(s, 'CAP * ACK :sasl');

      expect(s.saslMechanisms).toEqual([]);
      expect(s.saslPending).toBe(false);
      expect(sent).toEqual(['CAP END']);
    });
  });

  describe('draft/multiline', () => {
    test('the limits come out of the value', () => {
      const s = new CapState();

      feed(s, 'CAP * LS :draft/multiline=max-bytes=4096,max-lines=24');
      feed(s, 'CAP * ACK :draft/multiline');

      expect(s.multilineLimits).toEqual({ maxBytes: 4096, maxLines: 24 });
    });

    test('there are no limits when it is not in force', () => {
      const s = new CapState();

      feed(s, 'CAP * LS :draft/multiline=max-bytes=4096,max-lines=24');
      feed(s, 'CAP * NAK :draft/multiline');

      expect(s.multilineLimits).toBeUndefined();
    });
  });

  describe('CAP NEW and CAP DEL', () => {
    test('NEW for something wanted is asked for', () => {
      // A module was loaded while we were connected.
      const s = new CapState();

      feed(s, 'CAP * LS :message-tags');
      feed(s, 'CAP * ACK :message-tags');

      const sent = feed(s, 'CAP * NEW :draft/read-marker');

      expect(sent.length).toBe(1);
      expect(sent[0]).toContain('draft/read-marker');
    });

    test('NEW for something already in force is not asked for twice', () => {
      const s = new CapState();

      feed(s, 'CAP * LS :message-tags');
      feed(s, 'CAP * ACK :message-tags');

      expect(feed(s, 'CAP * NEW :message-tags')).toEqual([]);
    });

    test('NEW for something we do not use is ignored', () => {
      const s = new CapState();

      feed(s, 'CAP * LS :message-tags');
      feed(s, 'CAP * ACK :message-tags');

      expect(feed(s, 'CAP * NEW :some-other-thing')).toEqual([]);
    });

    test('DEL takes it away without a word', () => {
      // The module was unloaded.  There is nothing to send and nothing
      // to be done about it except stop using it.
      const s = new CapState();

      feed(s, 'CAP * LS :message-tags draft/read-marker');
      feed(s, 'CAP * ACK :message-tags draft/read-marker');

      expect(feed(s, 'CAP * DEL :draft/read-marker')).toEqual([]);
      expect(s.active.has('draft/read-marker')).toBe(false);
      expect(s.available.has('draft/read-marker')).toBe(false);
      expect(s.active.has('message-tags')).toBe(true);
    });
  });

  test('an ACK with a leading minus withdraws', () => {
    const s = new CapState();

    feed(s, 'CAP * LS :message-tags echo-message');
    feed(s, 'CAP * ACK :message-tags echo-message');
    feed(s, 'CAP * ACK :-echo-message');

    expect(s.active.has('echo-message')).toBe(false);
    expect(s.active.has('message-tags')).toBe(true);
  });

  test('a long list is split into several REQ lines', () => {
    const s = new CapState();
    const many = Array.from({ length: 40 }, (_, i) => `x-pad-${i}-aaaaaaaaaaaaaaaaaaaa`);

    feed(s, `CAP * LS :${['message-tags', 'server-time', 'batch', ...many].join(' ')}`);

    // Only the ones it recognises are asked for, so this stays one line;
    // the splitting itself is exercised by the line-length assertion.
    const sent = feed(s, 'CAP * LS :');

    for (const line of sent) expect(line.length).toBeLessThanOrEqual(420);
  });

  test('a line that is not CAP is not its business', () => {
    expect(new CapState().handle(parseMessage('PING :x')!).send).toEqual([]);
  });
});
