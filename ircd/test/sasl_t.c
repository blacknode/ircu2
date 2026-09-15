/* sasl_t.c - Test the SASL mechanism register and the client exchange.
 *
 * sasl.c is the shape of an authentication and not the decision, which is
 * what makes this testable without a server: no client, no socket, no
 * database.  What is checked here is the half that is identical on every
 * deployment -- who may register a mechanism, how the chunking works, what
 * PLAIN and EXTERNAL make of a message, and that a malformed one is
 * refused rather than guessed at.
 *
 * The other half, whether the credential is any good, is the identity
 * module's and is not here on purpose.
 */

#include "sasl.h"
#include "ircd_base64.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Two module handles are enough for per-module teardown; the register only
 * ever compares these pointers.
 */
static struct ModuleHandle* const MOD_A = (struct ModuleHandle*) 0x1;
static struct ModuleHandle* const MOD_B = (struct ModuleHandle*) 0x2;

/** A mechanism that asks a question before it answers one. */
static enum SaslStep step_twostep(struct SaslSession* ses, const char* in,
                                  size_t inlen)
{
  if (ses->ss_steps == 1) {
    memcpy(ses->ss_out, "challenge", 9);
    ses->ss_outlen = 9;
    return SASL_STEP_CHALLENGE;
  }

  (void) in;
  (void) inlen;
  strcpy(ses->ss_authcid, "two@example.org");
  return SASL_STEP_CREDENTIAL;
}

/** A mechanism that refuses everything. */
static enum SaslStep step_never(struct SaslSession* ses, const char* in,
                                size_t inlen)
{
  (void) ses; (void) in; (void) inlen;
  return SASL_STEP_FAIL;
}

/** One SASL_CHUNKLEN-long slice of \a wire starting at \a off. */
static const char* b64_chunk(const char* wire, size_t off)
{
  static char chunk[SASL_CHUNKLEN + 1];

  memcpy(chunk, wire + off, SASL_CHUNKLEN);
  chunk[SASL_CHUNKLEN] = '\0';

  return chunk;
}

/** Encode \a s as base64 into a static buffer. */
static const char* b64(const char* s, size_t len)
{
  static char out[SASL_WIRE_MAX + 1];

  assert(ircd_base64_encode(s, len, out, sizeof(out)) >= 0);

  return out;
}

/** The core brings PLAIN and EXTERNAL, and nothing else. */
static void test_core_mechanisms(void)
{
  sasl_init();

  assert(sasl_count() == 2);
  assert(sasl_find("PLAIN") != NULL);
  assert(sasl_find("EXTERNAL") != NULL);
  assert(sasl_find("SCRAM-SHA-256") == NULL);

  /* Looked up case-insensitively, like everything else a client types. */
  assert(sasl_find("plain") == sasl_find("PLAIN"));
  assert(sasl_find("") == NULL);
  assert(sasl_find(NULL) == NULL);

  /* PLAIN sends a password in the clear; EXTERNAL sends nothing at all. */
  assert(sasl_find("PLAIN")->sm_flags & SASL_MECH_NEEDS_TLS);
  assert(!(sasl_find("EXTERNAL")->sm_flags & SASL_MECH_NEEDS_TLS));

  /* Both are the core's, so no module owns them. */
  assert(sasl_find("PLAIN")->sm_owner == NULL);
  assert(sasl_module_count(MOD_A) == 0);

  printf("ok - the core registers PLAIN and EXTERNAL\n");
}

/** Names, and what is not a name. */
static void test_names(void)
{
  char toolong[SASLMECHLEN + 3];

  assert(sasl_name_valid("PLAIN"));
  assert(sasl_name_valid("SCRAM-SHA-256"));
  assert(sasl_name_valid("OAUTHBEARER"));
  assert(sasl_name_valid("X_MY_MECH"));
  assert(sasl_name_valid("A"));

  /* RFC 4422 names are uppercase; lowercase is normalised on the way in,
   * not accepted as a distinct name. */
  assert(!sasl_name_valid("plain"));
  assert(!sasl_name_valid(""));
  assert(!sasl_name_valid(NULL));
  assert(!sasl_name_valid("HAS SPACE"));
  assert(!sasl_name_valid("HAS/SLASH"));
  assert(!sasl_name_valid("HAS.DOT"));

  memset(toolong, 'A', sizeof(toolong) - 1);
  toolong[sizeof(toolong) - 1] = '\0';
  assert(!sasl_name_valid(toolong));

  sasl_init();
  assert(!sasl_register(MOD_A, toolong, 0, step_never));
  assert(!sasl_register(MOD_A, "BAD NAME", 0, step_never));
  assert(!sasl_register(MOD_A, "OK", 0, NULL));
  assert(sasl_count() == 2);

  printf("ok - mechanism names follow RFC 4422\n");
}

/** A module registers, and its registration goes away with it. */
static void test_module_mechanisms(void)
{
  sasl_init();

  assert(sasl_register(MOD_A, "SCRAM-SHA-256", 0, step_twostep));
  assert(sasl_count() == 3);
  assert(sasl_module_count(MOD_A) == 1);

  /* The name is uppercased, so two spellings are one mechanism. */
  assert(sasl_register(MOD_B, "oauthbearer", 0, step_twostep));
  assert(sasl_find("OAUTHBEARER") != NULL);
  assert(!sasl_register(MOD_A, "OAUTHBEARER", 0, step_twostep));
  assert(sasl_count() == 4);

  /* Nobody removes somebody else's, and nobody removes the core's: a
   * module that could withdraw PLAIN could lock a network out. */
  assert(!sasl_unregister(MOD_A, "OAUTHBEARER"));
  assert(!sasl_unregister(MOD_A, "PLAIN"));
  assert(!sasl_unregister(NULL, "SCRAM-SHA-256"));
  assert(sasl_count() == 4);

  assert(sasl_unregister(MOD_A, "SCRAM-SHA-256"));
  assert(sasl_count() == 3);
  assert(sasl_module_count(MOD_A) == 0);

  sasl_drop_module(MOD_B);
  assert(sasl_count() == 2);
  assert(sasl_find("OAUTHBEARER") == NULL);

  printf("ok - a module's mechanisms are its own, and leave with it\n");
}

/** The advertised list is sorted, so two servers agree on it. */
static void test_mechanisms_str(void)
{
  char buf[256];
  char small[8];

  sasl_init();
  sasl_mechanisms_str(buf, sizeof(buf));
  assert(0 == strcmp(buf, "EXTERNAL,PLAIN"));

  /* Registered last, listed in its place: the value must not depend on
   * the order the modules happened to load in. */
  assert(sasl_register(MOD_A, "ANONYMOUS", 0, step_never));
  sasl_mechanisms_str(buf, sizeof(buf));
  assert(0 == strcmp(buf, "ANONYMOUS,EXTERNAL,PLAIN"));

  /* A name that does not fit is left out whole, never cut in half: half a
   * mechanism name in a CAP LS is one a client will ask for and nobody
   * has.  Shorter ones still go in, so the result depends only on the set
   * and the buffer and not on where the buffer ran out. */
  sasl_mechanisms_str(small, sizeof(small));
  assert(0 == strcmp(small, "PLAIN"));

  sasl_close();
  assert(sasl_count() == 0);
  assert(sasl_mechanisms_str(buf, sizeof(buf)) == 0);
  assert(buf[0] == '\0');

  printf("ok - the advertised list is sorted and never truncated\n");
}

/** Starting an exchange, and what cannot start one. */
static void test_begin(void)
{
  struct SaslSession ses;

  sasl_init();

  sasl_session_init(&ses);
  assert(sasl_session_begin(&ses, "NOSUCH") == -1);

  /* PLAIN puts the password on the wire; without TLS it is refused here
   * rather than after the password has been sent. */
  assert(sasl_session_begin(&ses, "PLAIN") == -2);

  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);
  assert(ses.ss_mech == sasl_find("PLAIN"));

  /* EXTERNAL needs no TLS flag of its own: without a certificate there is
   * no fingerprint, and that is what it checks. */
  sasl_session_init(&ses);
  assert(sasl_session_begin(&ses, "EXTERNAL") == 0);

  sasl_session_clear(&ses);
  printf("ok - a mechanism that needs TLS is refused without it\n");
}

/** PLAIN, with and without an authzid. */
static void test_plain(void)
{
  struct SaslSession ses;
  char msg[128];
  size_t len;

  sasl_init();

  /* authzid NUL authcid NUL password, RFC 4616. */
  memcpy(msg, "\0maria@example.org\0secreto", 26);
  len = 26;

  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);
  assert(sasl_session_input(&ses, b64(msg, len)) == SASL_CREDENTIAL);
  assert(0 == strcmp(ses.ss_authcid, "maria@example.org"));
  assert(0 == strcmp(ses.ss_authzid, ""));
  assert(0 == strcmp(ses.ss_secret, "secreto"));
  assert(ses.ss_secretlen == 7);
  sasl_session_clear(&ses);

  /* With an authzid: which of the email's accounts to log into. */
  memcpy(msg, "maria\0maria@example.org\0secreto", 31);
  len = 31;

  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);
  assert(sasl_session_input(&ses, b64(msg, len)) == SASL_CREDENTIAL);
  assert(0 == strcmp(ses.ss_authzid, "maria"));
  assert(0 == strcmp(ses.ss_authcid, "maria@example.org"));
  sasl_session_clear(&ses);

  /* A password may contain anything, NUL included: it is taken by length
   * and not by scanning for a terminator. */
  memcpy(msg, "\0a@b\0pa\0ss", 10);
  len = 10;

  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);
  assert(sasl_session_input(&ses, b64(msg, len)) == SASL_CREDENTIAL);
  assert(ses.ss_secretlen == 5);
  assert(0 == memcmp(ses.ss_secret, "pa\0ss", 5));
  sasl_session_clear(&ses);

  printf("ok - PLAIN splits authzid, authcid and password\n");
}

/** What PLAIN must refuse. */
static void test_plain_refuses_nonsense(void)
{
  struct SaslSession ses;
  char msg[512];

  sasl_init();

#define PLAIN_REFUSES(buf, n)                                       \
  do {                                                              \
    sasl_session_init(&ses);                                        \
    ses.ss_tls = 1;                                                 \
    assert(sasl_session_begin(&ses, "PLAIN") == 0);                 \
    assert(sasl_session_input(&ses, b64((buf), (n)))               \
           == SASL_BAD_INPUT);                                      \
    sasl_session_clear(&ses);                                       \
  } while (0)

  /* No separators at all. */
  PLAIN_REFUSES("nonsense", 8);
  /* Only one separator. */
  memcpy(msg, "\0a@b", 4);
  PLAIN_REFUSES(msg, 4);
  /* Empty authcid. */
  memcpy(msg, "\0\0secret", 8);
  PLAIN_REFUSES(msg, 8);
  /* Empty password. */
  memcpy(msg, "\0a@b\0", 5);
  PLAIN_REFUSES(msg, 5);
  /* An authzid longer than a nick can be. */
  memset(msg, 'n', sizeof(msg));
  msg[NICKLEN + 5] = '\0';
  memcpy(msg + NICKLEN + 6, "a@b\0x", 5);
  PLAIN_REFUSES(msg, NICKLEN + 11);

#undef PLAIN_REFUSES

  /* Not base64 at all. */
  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);
  assert(sasl_session_input(&ses, "not!base64") == SASL_BAD_INPUT);
  sasl_session_clear(&ses);

  printf("ok - PLAIN refuses a message it cannot read\n");
}

/** EXTERNAL proves nothing without a certificate. */
static void test_external(void)
{
  struct SaslSession ses;

  sasl_init();

  /* With a fingerprint and an empty message: the default account. */
  sasl_session_init(&ses);
  ses.ss_tls = 1;
  strcpy(ses.ss_fingerprint, "aa:bb");
  assert(sasl_session_begin(&ses, "EXTERNAL") == 0);
  assert(sasl_session_input(&ses, "+") == SASL_CREDENTIAL);
  assert(0 == strcmp(ses.ss_authzid, ""));
  assert(ses.ss_secretlen == 0);
  sasl_session_clear(&ses);

  /* With a message: which account. */
  sasl_session_init(&ses);
  ses.ss_tls = 1;
  strcpy(ses.ss_fingerprint, "aa:bb");
  assert(sasl_session_begin(&ses, "EXTERNAL") == 0);
  assert(sasl_session_input(&ses, b64("maria", 5)) == SASL_CREDENTIAL);
  assert(0 == strcmp(ses.ss_authzid, "maria"));
  sasl_session_clear(&ses);

  /* Without one, there is nothing to prove with. */
  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "EXTERNAL") == 0);
  assert(sasl_session_input(&ses, "+") == SASL_BAD_INPUT);
  sasl_session_clear(&ses);

  printf("ok - EXTERNAL needs a certificate to mean anything\n");
}

/** The chunking rules of the IRCv3 SASL specification. */
static void test_chunking(void)
{
  struct SaslSession ses;
  char big[SASL_MESSAGE_MAX];
  char wire[SASL_WIRE_MAX + 1];
  char chunk[SASL_CHUNKLEN + 1];
  char toolong[SASL_CHUNKLEN + 2];
  size_t plainlen;
  size_t wirelen;
  size_t off;

  sasl_init();

  /* A message of exactly 300 base64 characters arrives in one line: 300
   * is short of the chunk size, so it is complete. */
  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);
  memcpy(big, "\0a@b\0x", 6);
  assert(sasl_session_input(&ses, b64(big, 6)) == SASL_CREDENTIAL);
  sasl_session_clear(&ses);

  /* A long one arrives in 400-character pieces, and only the short last
   * piece completes it. */
  plainlen = 0;
  memcpy(big + plainlen, "\0", 1); plainlen += 1;
  memcpy(big + plainlen, "a@b", 3); plainlen += 3;
  memcpy(big + plainlen, "\0", 1); plainlen += 1;
  memset(big + plainlen, 'p', SASL_SECRET_MAX);
  plainlen += SASL_SECRET_MAX;

  assert(ircd_base64_encode(big, plainlen, wire, sizeof(wire)) > 0);
  wirelen = strlen(wire);
  assert(wirelen > SASL_CHUNKLEN);

  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);

  for (off = 0; off + SASL_CHUNKLEN < wirelen; off += SASL_CHUNKLEN) {
    memcpy(chunk, wire + off, SASL_CHUNKLEN);
    chunk[SASL_CHUNKLEN] = '\0';
    assert(sasl_session_input(&ses, chunk) == SASL_NEED_MORE);
  }

  assert(sasl_session_input(&ses, wire + off) == SASL_CREDENTIAL);
  assert(ses.ss_secretlen == SASL_SECRET_MAX);
  sasl_session_clear(&ses);

  /* '*' aborts, at any point. */
  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);
  memcpy(chunk, wire, SASL_CHUNKLEN);
  chunk[SASL_CHUNKLEN] = '\0';
  assert(sasl_session_input(&ses, chunk) == SASL_NEED_MORE);
  assert(sasl_session_input(&ses, "*") == SASL_ABORTED);
  sasl_session_clear(&ses);

  /* A line longer than the chunk size is not a chunk at all: the spec
   * says 400 means "more follows", so 401 is a client doing something
   * else, and guessing which would be inventing a second encoding. */
  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);
  memset(toolong, 'A', SASL_CHUNKLEN + 1);
  toolong[SASL_CHUNKLEN + 1] = '\0';
  assert(sasl_session_input(&ses, toolong) == SASL_BAD_INPUT);
  sasl_session_clear(&ses);

  printf("ok - messages arrive in 400-character pieces\n");
}

/** A message longer than the server will assemble is refused. */
static void test_too_long(void)
{
  struct SaslSession ses;
  char chunk[SASL_CHUNKLEN + 1];
  int i;

  sasl_init();

  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);

  memset(chunk, 'A', SASL_CHUNKLEN);
  chunk[SASL_CHUNKLEN] = '\0';

  /* Enough pieces to go past SASL_WIRE_MAX; the one that would is the one
   * that is refused, and the exchange does not silently truncate. */
  for (i = 0; i < 100; i++) {
    enum SaslResult r = sasl_session_input(&ses, chunk);

    if (r == SASL_TOO_LONG)
      break;

    assert(r == SASL_NEED_MORE);
  }

  assert(i < 100);
  sasl_session_clear(&ses);

  /* And one that fits on the wire but decodes to more than the server
   * will assemble is "too long" too, not "malformed": the two are
   * different problems and a client cannot act on the wrong one. */
  {
    char plain[SASL_MESSAGE_MAX + 1];
    char wire[SASL_WIRE_MAX + 1];
    size_t off;

    memset(plain, 'x', sizeof(plain));
    assert(ircd_base64_encode(plain, sizeof(plain), wire, sizeof(wire)) > 0);
    assert(strlen(wire) <= SASL_WIRE_MAX);

    sasl_session_init(&ses);
    ses.ss_tls = 1;
    assert(sasl_session_begin(&ses, "PLAIN") == 0);

    for (off = 0; off + SASL_CHUNKLEN < strlen(wire); off += SASL_CHUNKLEN)
      assert(sasl_session_input(&ses, b64_chunk(wire, off)) == SASL_NEED_MORE);

    assert(sasl_session_input(&ses, wire + off) == SASL_TOO_LONG);
    sasl_session_clear(&ses);
  }

  printf("ok - an oversized message is refused, not truncated\n");
}

/** A mechanism with more than one round. */
static void test_multi_step(void)
{
  struct SaslSession ses;

  sasl_init();
  assert(sasl_register(MOD_A, "TWOSTEP", 0, step_twostep));

  sasl_session_init(&ses);
  assert(sasl_session_begin(&ses, "TWOSTEP") == 0);
  assert(sasl_session_input(&ses, "+") == SASL_CHALLENGE);
  assert(ses.ss_outlen == 9);
  assert(0 == memcmp(ses.ss_out, "challenge", 9));
  assert(sasl_session_input(&ses, b64("answer", 6)) == SASL_CREDENTIAL);
  assert(0 == strcmp(ses.ss_authcid, "two@example.org"));
  sasl_session_clear(&ses);

  printf("ok - a mechanism may ask before it answers\n");
}

/** Starting over does not carry the last attempt into the next. */
static void test_restart_is_clean(void)
{
  struct SaslSession ses;
  char msg[64];

  sasl_init();

  memcpy(msg, "maria\0maria@example.org\0secreto", 31);

  sasl_session_init(&ses);
  ses.ss_tls = 1;
  strcpy(ses.ss_fingerprint, "aa:bb");
  assert(sasl_session_begin(&ses, "PLAIN") == 0);
  assert(sasl_session_input(&ses, b64(msg, 31)) == SASL_CREDENTIAL);
  assert(ses.ss_secretlen == 7);

  /* A client that failed and tries another mechanism must not inherit the
   * password, the identity or half a message from the attempt before. */
  assert(sasl_session_begin(&ses, "EXTERNAL") == 0);
  assert(ses.ss_secretlen == 0);
  assert(ses.ss_secret[0] == '\0');
  assert(ses.ss_authcid[0] == '\0');
  assert(ses.ss_authzid[0] == '\0');
  assert(ses.ss_steps == 0);

  /* The connection's own details survive: they describe the socket, not
   * the attempt. */
  assert(ses.ss_tls == 1);
  assert(0 == strcmp(ses.ss_fingerprint, "aa:bb"));

  sasl_session_clear(&ses);
  printf("ok - a second attempt starts from nothing\n");
}

/** Clearing a session wipes the password it held. */
static void test_clear_wipes(void)
{
  struct SaslSession ses;
  char msg[64];

  sasl_init();
  memcpy(msg, "\0a@b\0hunter2", 12);

  sasl_session_init(&ses);
  ses.ss_tls = 1;
  assert(sasl_session_begin(&ses, "PLAIN") == 0);
  assert(sasl_session_input(&ses, b64(msg, 12)) == SASL_CREDENTIAL);
  assert(0 == strcmp(ses.ss_secret, "hunter2"));

  sasl_session_clear(&ses);

  assert(ses.ss_secretlen == 0);
  assert(NULL == memchr(ses.ss_secret, 'h', sizeof(ses.ss_secret)));
  assert(ses.ss_mech == NULL);

  printf("ok - clearing a session wipes the password\n");
}

/** Input before a mechanism was chosen goes nowhere. */
static void test_input_without_mechanism(void)
{
  struct SaslSession ses;

  sasl_init();
  sasl_session_init(&ses);

  assert(sasl_session_input(&ses, "+") == SASL_BAD_INPUT);
  assert(sasl_session_input(&ses, b64("x", 1)) == SASL_BAD_INPUT);

  /* Except '*', which a client may send at any point to give up. */
  assert(sasl_session_input(&ses, "*") == SASL_ABORTED);

  sasl_session_clear(&ses);
  printf("ok - nothing is read before a mechanism is chosen\n");
}

int main(void)
{
  test_core_mechanisms();
  test_names();
  test_module_mechanisms();
  test_mechanisms_str();
  test_begin();
  test_plain();
  test_plain_refuses_nonsense();
  test_external();
  test_chunking();
  test_too_long();
  test_multi_step();
  test_restart_is_clean();
  test_clear_wipes();
  test_input_without_mechanism();

  sasl_close();

  printf("ok - sasl_t\n");
  return 0;
}
