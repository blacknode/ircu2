/* mail_t.c - Test the mail provider register and the verification token.
 *
 * Two halves, and the second is the one that matters.  The register is
 * the shape cache.c and db.c have, tested the same way: a provider that
 * is missing is not an error anybody has to handle, and one that goes
 * away with messages outstanding fails them rather than leaving somebody
 * waiting.
 *
 * The token is a signature, so what has to be pinned down is every way of
 * getting one wrong: an address the server never signed, a byte flipped
 * in the payload, a byte flipped in the tag, a token from a network with
 * another key, one that was good yesterday, and one minted for something
 * else entirely.  All six have to fail, and the one legitimate token has
 * to come back with the address it was minted for, byte for byte.
 */

#include "mail.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_base64.h"
#include "ircd_string.h"
#include "ircd_token.h"
#include "ircd_vhost.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* --- stubs ---------------------------------------------------------- */

int feature_int(enum Feature feat)
{
  (void) feat;
  return 0;
}

const char* feature_str(enum Feature feat)
{
  (void) feat;
  return "";
}

/* ircd_snprintf() can render a client; nothing here ever asks it to. */
const char* visible_username(const struct Client* cptr)
{
  (void) cptr;
  return "";
}

/* Two module handles; the register only compares these pointers. */
static struct ModuleHandle* const MOD_A = (struct ModuleHandle*) 0x1;
static struct ModuleHandle* const MOD_B = (struct ModuleHandle*) 0x2;

/* --- what the fake provider was asked -------------------------------- */

static int prov_send_calls;
static int prov_cancel_calls;
static mail_id_t prov_last_id;
static char prov_last_to[MAIL_ADDRESS_MAX + 1];
static char prov_last_from[MAIL_ADDRESS_MAX + 1];
static char prov_last_subject[MAIL_SUBJECT_MAX + 1];
static char prov_last_body[MAIL_BODY_MAX + 1];
/** What mp_send returns. */
static enum MailError prov_accept = MAIL_OK;
/** When non-zero, the provider answers from inside the send. */
static int prov_answer_now;

static void prov_reset(void)
{
  prov_send_calls = prov_cancel_calls = 0;
  prov_last_id = 0;
  prov_last_to[0] = prov_last_from[0] = '\0';
  prov_last_subject[0] = prov_last_body[0] = '\0';
  prov_accept = MAIL_OK;
  prov_answer_now = 0;
}

static enum MailError prov_send(mail_id_t id, const struct MailMessage* msg)
{
  prov_send_calls++;
  prov_last_id = id;
  ircd_strncpy(prov_last_to, msg->mm_to, sizeof(prov_last_to) - 1);
  ircd_strncpy(prov_last_from, msg->mm_from, sizeof(prov_last_from) - 1);
  ircd_strncpy(prov_last_subject, msg->mm_subject,
               sizeof(prov_last_subject) - 1);
  ircd_strncpy(prov_last_body, msg->mm_body, sizeof(prov_last_body) - 1);

  if (prov_accept != MAIL_OK)
    return prov_accept;

  if (prov_answer_now)
    mail_complete(id, MAIL_OK, NULL);

  return MAIL_OK;
}

static void prov_cancel(mail_id_t id)
{
  (void) id;
  prov_cancel_calls++;
}

static const struct MailProvider provider = {
  "test", prov_send, prov_cancel
};

/* A provider with no cancel: the core must not require one. */
static const struct MailProvider provider_nocancel = {
  "nocancel", prov_send, NULL
};

/* --- what the caller was told ---------------------------------------- */

static int done_calls;
static enum MailError done_err;
static char done_detail[128];

static void done_reset(void)
{
  done_calls = 0;
  done_err = MAIL_OK;
  done_detail[0] = '\0';
}

static void on_done(enum MailError err, const char* detail, void* user)
{
  (void) user;
  done_calls++;
  done_err = err;
  ircd_strncpy(done_detail, detail ? detail : "", sizeof(done_detail) - 1);
}

/* --- helpers --------------------------------------------------------- */

/** A usable Mail{} block.
 *
 * The setters take ownership of what they are given, the way the parser
 * hands them strings it allocated.
 */
static void configure(void)
{
  const char* err = NULL;
  char* from = (char*) MyMalloc(32);

  strcpy(from, "noreply@example.net");

  mail_conf_clear();
  mail_conf_set_from(from);
  mail_conf_set_timeout(30);
  mail_conf_set_window(3600);

  assert(mail_conf_commit(&err));
}

/** Forget everything between tests. */
static void reset(void)
{
  mail_close();
  prov_reset();
  done_reset();
  configure();
}

/* --- the register ---------------------------------------------------- */

static void test_no_provider(void)
{
  reset();

  /* No provider is not an error anybody has to handle: the send is
   * refused, nothing is called back, and the caller is told why. */
  assert(!mail_available());
  assert(!mail_send(NULL, "her@example.net", "hello", "there", on_done, 0));
  assert(done_calls == 0);
  assert(mail_last_error() == MAIL_ERR_NOPROVIDER);
  assert(!strcmp(mail_provider_name(), "none"));

  printf("Passed: no provider is a refusal, not a callback\n");
}

static void test_register(void)
{
  reset();

  assert(mail_register_provider(MOD_A, &provider));
  assert(mail_available());
  assert(!strcmp(mail_provider_name(), "test"));

  /* One at a time: a second is refused rather than replacing the first,
   * so two mail modules are an error and not a coin flip. */
  assert(!mail_register_provider(MOD_B, &provider_nocancel));
  assert(!strcmp(mail_provider_name(), "test"));

  /* And a provider missing the one thing it is for. */
  {
    static const struct MailProvider broken = { "broken", NULL, NULL };

    mail_unregister_provider(MOD_A);
    assert(!mail_register_provider(MOD_A, &broken));
    assert(!mail_available());
  }

  printf("Passed: one provider, and it has to be able to send\n");
}

static void test_send_and_answer(void)
{
  reset();
  assert(mail_register_provider(MOD_A, &provider));

  {
    mail_id_t id = mail_send(NULL, "her@example.net", "Subject",
                             "Body text", on_done, 0);

    assert(id != 0);
    assert(prov_send_calls == 1);
    assert(mail_pending_count() == 1);

    /* The sender is the core's, from the block: a provider cannot forget
     * it and two providers cannot disagree about it. */
    assert(!strcmp(prov_last_from, "noreply@example.net"));
    assert(!strcmp(prov_last_to, "her@example.net"));
    assert(!strcmp(prov_last_subject, "Subject"));
    assert(!strcmp(prov_last_body, "Body text"));

    assert(mail_complete(id, MAIL_OK, NULL));
    assert(done_calls == 1);
    assert(done_err == MAIL_OK);
    assert(mail_pending_count() == 0);

    /* Consumed: a second answer is not an error, it is a no-op. */
    assert(!mail_complete(id, MAIL_OK, NULL));
    assert(done_calls == 1);
  }

  printf("Passed: a message goes out and comes back once\n");
}

static void test_refused_by_provider(void)
{
  reset();
  assert(mail_register_provider(MOD_A, &provider));

  prov_accept = MAIL_ERR_FAILED;

  /* Refused on the spot still goes through the callback: one path out of
   * a send, not two. */
  assert(!mail_send(NULL, "her@example.net", "s", "b", on_done, 0));
  assert(done_calls == 1);
  assert(done_err == MAIL_ERR_FAILED);
  assert(mail_pending_count() == 0);

  printf("Passed: a refusal is still an answer\n");
}

static void test_provider_answers_immediately(void)
{
  reset();
  assert(mail_register_provider(MOD_A, &provider));

  prov_answer_now = 1;

  /* A provider that answers from inside mp_send() is unusual and legal;
   * the entry must be off the list before the callback runs, or the
   * callback's own send would find it. */
  mail_send(NULL, "her@example.net", "s", "b", on_done, 0);
  assert(done_calls == 1);
  assert(done_err == MAIL_OK);
  assert(mail_pending_count() == 0);

  printf("Passed: an answer from inside the ask\n");
}

static void test_withdrawal_fails_what_is_out(void)
{
  reset();
  assert(mail_register_provider(MOD_A, &provider));

  mail_send(NULL, "her@example.net", "s", "b", on_done, 0);
  assert(mail_pending_count() == 1);

  mail_unregister_provider(MOD_A);

  assert(done_calls == 1);
  assert(done_err == MAIL_ERR_NOPROVIDER);
  assert(mail_pending_count() == 0);
  assert(!mail_available());

  printf("Passed: a provider that leaves answers what it owed\n");
}

static void test_module_cancel(void)
{
  reset();
  assert(mail_register_provider(MOD_A, &provider));

  mail_send(MOD_B, "her@example.net", "s", "b", on_done, 0);
  assert(mail_pending_count() == 1);

  /* The module that asked is gone.  Its message is dropped without a
   * callback -- there is nobody left to tell -- and the provider is told
   * to forget it. */
  mail_cancel_module(MOD_B);
  assert(done_calls == 0);
  assert(prov_cancel_calls == 1);
  assert(mail_pending_count() == 0);

  printf("Passed: a module that leaves takes its messages with it\n");
}

static void test_expiry(void)
{
  reset();
  assert(mail_register_provider(MOD_A, &provider));

  CurrentTime = 1000;
  mail_send(NULL, "her@example.net", "s", "b", on_done, 0);

  assert(mail_expire(1000) == 0);
  assert(done_calls == 0);

  assert(mail_expire(1000 + 31) == 1);
  assert(done_calls == 1);
  assert(done_err == MAIL_ERR_TIMEOUT);
  assert(prov_cancel_calls == 1);

  printf("Passed: a provider that never answers is given up on\n");
}

/* --- addresses ------------------------------------------------------- */

static void test_addresses(void)
{
  reset();

  assert(mail_address_valid("her@example.net"));
  assert(mail_address_valid("first.last+tag@sub.example.co.uk"));

  assert(!mail_address_valid(""));
  assert(!mail_address_valid("nobody"));
  assert(!mail_address_valid("@example.net"));
  assert(!mail_address_valid("her@"));
  assert(!mail_address_valid("her@localhost"));      /* no dot */
  assert(!mail_address_valid("her@.example.net"));
  assert(!mail_address_valid("her@example.net."));
  assert(!mail_address_valid("one@a.net,two@b.net"));
  assert(!mail_address_valid("her@a.net> <him@b.net"));

  /* The one that matters: a newline in an address is a header of
   * somebody else's choosing, and the address comes from a client. */
  assert(!mail_address_valid("her@example.net\nBcc: everyone@example.net"));
  assert(!mail_address_valid("her@example.net\r\nSubject: no"));
  assert(!mail_address_valid("her @example.net"));

  printf("Passed: what an address may be\n");
}

static void test_subject_may_not_carry_a_header(void)
{
  reset();
  assert(mail_register_provider(MOD_A, &provider));

  assert(!mail_send(NULL, "her@example.net", "hello\r\nBcc: x@y.net",
                    "body", on_done, 0));
  assert(prov_send_calls == 0);

  printf("Passed: a subject is one line\n");
}

/* --- the token ------------------------------------------------------- */

/** The key every server of a network shares. */
#define KEY_ONE "AbCdEfGhIjKl"
/** Another network's. */
#define KEY_TWO "ZZaabbccddee"

static void test_token_round_trip(void)
{
  char token[MAIL_TOKEN_MAX];
  char email[MAIL_ADDRESS_MAX + 1];

  reset();
  assert(vhost_set_key(KEY_ONE));

  assert(mail_token_make(token, sizeof(token), "her@example.net", 1000, 3600));
  assert(mail_token_check(token, email, sizeof(email), 1001) == MAIL_TOKEN_OK);
  assert(!strcmp(email, "her@example.net"));

  /* Nothing was stored: a second server, given the same key, recognises
   * the same token.  That is the whole reason it is signed and not kept. */
  assert(vhost_set_key(KEY_ONE));
  assert(mail_token_check(token, email, sizeof(email), 1001) == MAIL_TOKEN_OK);

  printf("Passed: a token says which address, and any server can read it\n");
}

static void test_token_expiry(void)
{
  char token[MAIL_TOKEN_MAX];
  char email[MAIL_ADDRESS_MAX + 1];

  reset();
  assert(vhost_set_key(KEY_ONE));

  assert(mail_token_make(token, sizeof(token), "her@example.net", 1000, 60));

  assert(mail_token_check(token, email, sizeof(email), 1059) == MAIL_TOKEN_OK);
  assert(mail_token_check(token, email, sizeof(email), 1060)
         == MAIL_TOKEN_EXPIRED);
  assert(mail_token_check(token, email, sizeof(email), 99999)
         == MAIL_TOKEN_EXPIRED);

  printf("Passed: a token stops working\n");
}

static void test_token_tampering(void)
{
  char token[MAIL_TOKEN_MAX];
  char broken[MAIL_TOKEN_MAX];
  char email[MAIL_ADDRESS_MAX + 1];
  size_t len;
  size_t i;
  int flipped;

  reset();
  assert(vhost_set_key(KEY_ONE));
  assert(mail_token_make(token, sizeof(token), "her@example.net", 1000, 3600));
  len = strlen(token);

  /* Every single character of the token, changed: not one of them may
   * still verify.  Which half it lands in -- the claim or the signature
   * -- is exactly what must not matter. */
  for (i = 0; i < len; i++) {
    if (token[i] == '.')
      continue;

    strcpy(broken, token);
    broken[i] = (token[i] == 'A') ? 'B' : 'A';

    if (!strcmp(broken, token))
      continue;

    flipped = mail_token_check(broken, email, sizeof(email), 1001);
    assert(flipped != MAIL_TOKEN_OK);
  }

  /* And the shapes that are not tokens at all. */
  assert(mail_token_check("", email, sizeof(email), 1001)
         == MAIL_TOKEN_MALFORMED);
  assert(mail_token_check("1.", email, sizeof(email), 1001)
         == MAIL_TOKEN_MALFORMED);
  assert(mail_token_check("2.abc.def", email, sizeof(email), 1001)
         == MAIL_TOKEN_MALFORMED);
  assert(mail_token_check("nonsense", email, sizeof(email), 1001)
         == MAIL_TOKEN_MALFORMED);

  printf("Passed: one changed character is one refused token\n");
}

static void test_token_is_per_network(void)
{
  char token[MAIL_TOKEN_MAX];
  char email[MAIL_ADDRESS_MAX + 1];

  reset();
  assert(vhost_set_key(KEY_ONE));
  assert(mail_token_make(token, sizeof(token), "her@example.net", 1000, 3600));

  /* Another network's key: the token is well formed and not ours. */
  assert(vhost_set_key(KEY_TWO));
  assert(mail_token_check(token, email, sizeof(email), 1001)
         == MAIL_TOKEN_BAD);

  printf("Passed: a token from another network is not ours\n");
}

static void test_token_needs_a_key(void)
{
  char token[MAIL_TOKEN_MAX];
  char payload64[128];
  char tag64[64];
  char email[MAIL_ADDRESS_MAX + 1];
  static const char payload[] = "9999999999:her@example.net";
  static const unsigned char tag[16] = { 0 };

  reset();

  /* Before any Security{} block has been read.  There is nothing to sign
   * with, and saying so is better than signing with nothing -- a token
   * signed with an empty key is one every network would accept. */
  assert(!vhost_have_key());
  assert(!mail_token_make(token, sizeof(token), "her@example.net", 1000, 60));

  /* A well-formed token, and still no key to check it against. */
  assert(ircd_base64_encode(payload, strlen(payload), payload64,
                            sizeof(payload64)) > 0);
  assert(ircd_base64_encode(tag, sizeof(tag), tag64, sizeof(tag64)) > 0);
  sprintf(token, "1.%s.%s", payload64, tag64);

  assert(mail_token_check(token, email, sizeof(email), 1001)
         == MAIL_TOKEN_NOKEY);

  printf("Passed: no key, no token\n");
}

static void test_token_long_address(void)
{
  char token[MAIL_TOKEN_MAX];
  char email[MAIL_ADDRESS_MAX + 1];
  char long_email[MAIL_ADDRESS_MAX + 2];
  size_t i;

  reset();
  assert(vhost_set_key(KEY_ONE));

  for (i = 0; i < MAIL_ADDRESS_MAX - 12; i++)
    long_email[i] = 'a';
  strcpy(long_email + i, "@example.net");

  assert(strlen(long_email) == MAIL_ADDRESS_MAX);
  assert(mail_token_make(token, sizeof(token), long_email, 1000, 3600));
  assert(mail_token_check(token, email, sizeof(email), 1001) == MAIL_TOKEN_OK);
  assert(!strcmp(email, long_email));

  printf("Passed: the longest address still fits in a token\n");
}

/* The label is what keeps one kind of token from being another.
 *
 * Two things sign with this key -- a verified address and an upload
 * ticket -- and neither may be presented as the other: a verification
 * mail that could be pasted into an upload would be an upload nobody
 * authorised.  Nothing but the label separates them, so this is where
 * that is pinned down.
 */
static void test_token_labels_do_not_mix(void)
{
  char token[TOKEN_MAX + 1];
  char payload[TOKEN_PAYLOAD_MAX + 1];

  reset();
  assert(vhost_set_key(KEY_ONE));

  assert(ircd_token_make(token, sizeof(token), "ircu-file-upload-v1",
                         "abcdef:maria:#files", 1000, 900));

  assert(ircd_token_check(token, "ircu-file-upload-v1", payload,
                          sizeof(payload), 1001) == TOKEN_OK);
  assert(!strcmp(payload, "abcdef:maria:#files"));

  /* The same bytes, asked about under another name. */
  assert(ircd_token_check(token, "ircu-mail-verify-v1", payload,
                          sizeof(payload), 1001) == TOKEN_BAD);

  /* And the mail side's own check, which is that label, refuses it too --
   * the two are not the same function with a different argument by
   * accident. */
  {
    char email[MAIL_ADDRESS_MAX + 1];

    assert(mail_token_check(token, email, sizeof(email), 1001)
           == MAIL_TOKEN_BAD);
  }

  printf("Passed: a token minted under one label is not one under another\n");
}

int main(int argc, char* argv[])
{
  (void) argc;
  (void) argv;

  test_no_provider();
  test_register();
  test_send_and_answer();
  test_refused_by_provider();
  test_provider_answers_immediately();
  test_withdrawal_fails_what_is_out();
  test_module_cancel();
  test_expiry();
  test_addresses();
  test_subject_may_not_carry_a_header();
  /* First, because there is no way to take a key back once one is
   * installed -- and a rehash that drops the Security{} block keeps the
   * key already in force, which is the server's behaviour and not an
   * accident of this test. */
  test_token_needs_a_key();

  test_token_round_trip();
  test_token_expiry();
  test_token_tampering();
  test_token_is_per_network();
  test_token_long_address();
  test_token_labels_do_not_mix();

  mail_close();

  printf("All mail tests passed\n");

  return 0;
}
