/* account_t.c - Test the identity provider register and the questions
 * in flight.
 *
 * ircd/account.c is the half of the account subsystem that asks and holds
 * and never dereferences a client, which is what makes this testable
 * without a server.  What is checked here is the contract a provider is
 * held to: one at a time, an answer exactly once, a question that outlives
 * its client is dropped rather than answered, a deadline that passes
 * counts as a failure, and unloading the module fails everything it still
 * owed.
 *
 * What an answer then does to a user is ircd/account_user.c's, and is not
 * here on purpose.
 */

#include "account.h"
#include "client.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_string.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* --- stubs ---------------------------------------------------------- */

/** How long a question may go unanswered, as the test wants it. */
static int stub_timeout = 10;
/** The guest prefix, as the test wants it. */
static const char* stub_guest_prefix = "guest-";

int feature_int(enum Feature feat)
{
  return feat == FEAT_ACCOUNT_TIMEOUT ? stub_timeout : 0;
}

const char* feature_str(enum Feature feat)
{
  return feat == FEAT_GUEST_PREFIX ? stub_guest_prefix : "";
}

/* A plain xorshift, so a guest name is drawn from something that moves.
 * ircrandom() lives in random.c, which pulls in MD5 and the parser's
 * error helpers -- a lot of server for four bytes of noise.
 */
unsigned int ircrandom(void)
{
  static unsigned int state = 2463534242u;

  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;

  return state;
}

/* Two module handles; the register only compares these pointers. */
static struct ModuleHandle* const MOD_A = (struct ModuleHandle*) 0x1;
static struct ModuleHandle* const MOD_B = (struct ModuleHandle*) 0x2;

/* Likewise two clients: account.c stores them and never reads them. */
static struct Client* const CLI_A = (struct Client*) 0x10;
static struct Client* const CLI_B = (struct Client*) 0x20;

/* --- what the fake provider was asked ------------------------------- */

static int prov_verify_calls;
static int prov_lookup_calls;
static int prov_list_calls;
static int prov_cancel_calls;
static account_id_t prov_last_id;
static char prov_last_authcid[ACCOUNT_EMAIL_MAX + 1];
static char prov_last_nick[NICKLEN + 1];
static char prov_last_email[ACCOUNT_EMAIL_MAX + 1];
/** When non-zero, the provider answers from inside the ask. */
static int prov_answer_now;

static void prov_reset(void)
{
  prov_verify_calls = prov_lookup_calls = prov_list_calls = 0;
  prov_cancel_calls = 0;
  prov_last_id = 0;
  prov_last_authcid[0] = '\0';
  prov_last_nick[0] = '\0';
  prov_last_email[0] = '\0';
  prov_answer_now = 0;
}

static void prov_verify(account_id_t id, const struct AccountRequest* req)
{
  prov_verify_calls++;
  prov_last_id = id;
  ircd_strncpy(prov_last_authcid, req->ar_authcid, ACCOUNT_EMAIL_MAX);

  if (prov_answer_now)
    account_complete(id, ACCOUNT_OK, "maria", "maria@example.org", 0);
}

static void prov_lookup(account_id_t id, const char* nick)
{
  prov_lookup_calls++;
  prov_last_id = id;
  ircd_strncpy(prov_last_nick, nick, NICKLEN);
}

static void prov_list(account_id_t id, const char* email)
{
  prov_list_calls++;
  prov_last_id = id;
  ircd_strncpy(prov_last_email, email, ACCOUNT_EMAIL_MAX);
}

static void prov_cancel(account_id_t id)
{
  prov_cancel_calls++;
  (void) id;
}

static const struct AccountProvider provider = {
  "test", prov_verify, prov_lookup, prov_list, prov_cancel
};

/** A second provider, to prove only one may register. */
static const struct AccountProvider provider_two = {
  "other", prov_verify, prov_lookup, prov_list, prov_cancel
};

/** One with a hole in it. */
static const struct AccountProvider provider_broken = {
  "broken", 0, prov_lookup, prov_list, prov_cancel
};

/** One that can verify but cannot list, which is also a hole. */
static const struct AccountProvider provider_no_list = {
  "nolist", prov_verify, prov_lookup, 0, prov_cancel
};

/* --- what the core was told ----------------------------------------- */

static int done_calls;
static enum AccountResult done_result;
static struct Client* done_client;
static char done_nick[NICKLEN + 1];
static char done_email[ACCOUNT_EMAIL_MAX + 1];
static void* done_data;

static int owner_calls;
static enum AccountOwner owner_result;
static char owner_nick[NICKLEN + 1];

static int list_calls;
static enum AccountResult list_result;
static struct Client* list_client;
static unsigned int list_count;
static char list_first[NICKLEN + 1];
static int list_first_default;

static void on_list(struct Client* cptr, enum AccountResult result,
                    const struct AccountEntry* entries, unsigned int count,
                    const char* reason, void* data)
{
  (void) reason; (void) data;
  list_calls++;
  list_result = result;
  list_client = cptr;
  list_count = count;
  list_first[0] = '\0';
  list_first_default = 0;

  if (entries && count) {
    ircd_strncpy(list_first, entries[0].ae_nick, NICKLEN);
    list_first_default = entries[0].ae_default;
  }
}

static void done_reset(void)
{
  done_calls = owner_calls = list_calls = 0;
  list_client = 0;
  list_count = 0;
  list_first[0] = '\0';
  list_first_default = 0;
  list_result = ACCOUNT_OK;
  done_result = ACCOUNT_OK;
  done_client = 0;
  done_nick[0] = done_email[0] = owner_nick[0] = '\0';
  done_data = 0;
  owner_result = ACCOUNT_NICK_UNKNOWN;
}

static void on_done(struct Client* cptr, enum AccountResult result,
                    const char* nick, const char* email, const char* reason,
                    void* data)
{
  (void) reason;
  done_calls++;
  done_result = result;
  done_client = cptr;
  done_data = data;
  ircd_strncpy(done_nick, nick ? nick : "", NICKLEN);
  ircd_strncpy(done_email, email ? email : "", ACCOUNT_EMAIL_MAX);
}

static void on_owner(struct Client* cptr, enum AccountOwner owner,
                     const char* nick, void* data)
{
  (void) cptr; (void) data;
  owner_calls++;
  owner_result = owner;
  ircd_strncpy(owner_nick, nick ? nick : "", NICKLEN);
}

static struct AccountRequest a_request(const char* authcid)
{
  struct AccountRequest req;

  memset(&req, 0, sizeof(req));
  req.ar_mech = "PLAIN";
  req.ar_authcid = authcid;
  req.ar_authzid = "";
  req.ar_secret = "secreto";
  req.ar_secretlen = 7;
  req.ar_fingerprint = "";
  req.ar_ip = "127.0.0.1";
  req.ar_tls = 1;

  return req;
}

static void setup(void)
{
  account_close();
  prov_reset();
  done_reset();
}

/** One provider at a time, and it must be able to answer. */
static void test_register(void)
{
  setup();

  assert(!account_have_provider());
  assert(0 == strcmp(account_provider_name(), "none"));

  /* A provider that cannot verify is not a provider. */
  assert(!account_register_provider(MOD_A, &provider_broken));
  /* Nor is one that can verify but cannot list: ACCOUNT LIST would then
   * answer "the identity service is not available" on a server where
   * identity works, which is the one answer a user cannot tell from an
   * outage. */
  assert(!account_register_provider(MOD_A, &provider_no_list));
  assert(!account_register_provider(MOD_A, 0));
  assert(!account_have_provider());

  assert(account_register_provider(MOD_A, &provider));
  assert(account_have_provider());
  assert(0 == strcmp(account_provider_name(), "test"));

  /* A second is refused rather than replacing the first: two identity
   * modules in ircd.conf should be an error, not a coin flip. */
  assert(!account_register_provider(MOD_B, &provider_two));
  assert(0 == strcmp(account_provider_name(), "test"));

  /* Only whoever registered it withdraws it. */
  account_unregister_provider(MOD_B);
  assert(account_have_provider());

  account_unregister_provider(MOD_A);
  assert(!account_have_provider());

  printf("ok - one identity provider at a time\n");
}

/** With nobody registered, nothing is asked and nothing is promised. */
static void test_no_provider(void)
{
  struct AccountRequest req = a_request("maria@example.org");

  setup();

  assert(account_verify(CLI_A, &req, on_done, 0) == 0);
  assert(account_lookup(CLI_A, "maria", on_owner, 0) == 0);
  assert(account_pending_count() == 0);

  /* The caller is told by the return value, not by a callback: a
   * callback before the call returns is a reentrancy the caller has no
   * reason to expect. */
  assert(done_calls == 0);
  assert(owner_calls == 0);

  printf("ok - with no provider nothing is asked and nothing calls back\n");
}

/** A credential goes to the provider and the answer comes back once. */
static void test_verify(void)
{
  struct AccountRequest req = a_request("maria@example.org");
  account_id_t id;

  setup();
  assert(account_register_provider(MOD_A, &provider));

  id = account_verify(CLI_A, &req, on_done, (void*) 0x99);
  assert(id != 0);
  assert(prov_verify_calls == 1);
  assert(prov_last_id == id);
  assert(0 == strcmp(prov_last_authcid, "maria@example.org"));

  /* Nothing has been decided yet. */
  assert(done_calls == 0);
  assert(account_pending_count() == 1);

  assert(account_complete(id, ACCOUNT_OK, "maria", "maria@example.org", 0));
  assert(done_calls == 1);
  assert(done_result == ACCOUNT_OK);
  assert(done_client == CLI_A);
  assert(done_data == (void*) 0x99);
  assert(0 == strcmp(done_nick, "maria"));
  assert(0 == strcmp(done_email, "maria@example.org"));
  assert(account_pending_count() == 0);

  /* The same handle cannot answer twice. */
  assert(!account_complete(id, ACCOUNT_ERR_CREDENTIAL, 0, 0, 0));
  assert(done_calls == 1);

  printf("ok - a credential is answered exactly once\n");
}

/** A provider with a cache may answer before the ask returns. */
static void test_verify_answers_immediately(void)
{
  struct AccountRequest req = a_request("maria@example.org");
  account_id_t id;

  setup();
  assert(account_register_provider(MOD_A, &provider));
  prov_answer_now = 1;

  id = account_verify(CLI_A, &req, on_done, 0);

  /* The handle still comes back, and the answer has already arrived: the
   * caller must not be handed a handle that was freed underneath it. */
  assert(id != 0);
  assert(done_calls == 1);
  assert(done_result == ACCOUNT_OK);
  assert(account_pending_count() == 0);

  printf("ok - a provider may answer from inside the ask\n");
}

/** A nickname lookup is a different question with a different answer. */
static void test_lookup(void)
{
  account_id_t id;

  setup();
  assert(account_register_provider(MOD_A, &provider));

  assert(account_lookup(CLI_A, "", on_owner, 0) == 0);
  assert(account_lookup(CLI_A, 0, on_owner, 0) == 0);
  assert(prov_lookup_calls == 0);

  id = account_lookup(CLI_A, "maria", on_owner, 0);
  assert(id != 0);
  assert(prov_lookup_calls == 1);
  assert(0 == strcmp(prov_last_nick, "maria"));

  /* And the two kinds of answer do not cross: completing a lookup as a
   * credential, or the other way round, finds nothing. */
  assert(!account_complete(id, ACCOUNT_OK, "maria", 0, 0));
  assert(done_calls == 0);
  assert(account_pending_count() == 1);

  assert(account_complete_owner(id, ACCOUNT_NICK_REGISTERED));
  assert(owner_calls == 1);
  assert(owner_result == ACCOUNT_NICK_REGISTERED);
  assert(0 == strcmp(owner_nick, "maria"));
  assert(account_pending_count() == 0);

  printf("ok - a nickname lookup is answered on its own\n");
}

/** A client that leaves takes its questions with it, silently. */
static void test_client_gone(void)
{
  struct AccountRequest req = a_request("maria@example.org");
  account_id_t id;

  setup();
  assert(account_register_provider(MOD_A, &provider));

  id = account_verify(CLI_A, &req, on_done, 0);
  assert(account_verify(CLI_B, &req, on_done, 0) != 0);
  assert(account_pending_count() == 2);

  account_cancel_client(CLI_B);
  assert(account_pending_count() == 1);
  assert(prov_cancel_calls == 1);

  /* Dropped, not failed: there is nobody left to tell, and the callback
   * would reach into a client that is being freed. */
  assert(done_calls == 0);

  account_cancel_client(CLI_A);
  assert(account_pending_count() == 0);
  assert(prov_cancel_calls == 2);
  assert(done_calls == 0);

  /* And a late answer from the provider finds nothing. */
  assert(!account_complete(id, ACCOUNT_OK, "maria", 0, 0));

  printf("ok - a question dies with its client, without an answer\n");
}

/** A deadline that passes is a failure, not a silence. */
static void test_expire(void)
{
  struct AccountRequest req = a_request("maria@example.org");
  account_id_t id;
  time_t deadline;

  setup();
  assert(account_register_provider(MOD_A, &provider));

  CurrentTime = 1000;
  deadline = CurrentTime + stub_timeout;

  id = account_verify(CLI_A, &req, on_done, 0);
  assert(id != 0);

  assert(account_expire(deadline - 1) == 0);
  assert(done_calls == 0);
  assert(account_pending_count() == 1);

  /* The deadline is inclusive. */
  assert(account_expire(deadline) == 1);
  assert(done_calls == 1);
  assert(done_result == ACCOUNT_ERR_TIMEOUT);
  assert(account_pending_count() == 0);
  assert(prov_cancel_calls == 1);

  assert(!account_complete(id, ACCOUNT_OK, "maria", 0, 0));

  printf("ok - a question that is never answered times out\n");
}

/** Withdrawing the provider fails everything it still owed. */
static void test_provider_withdrawn(void)
{
  struct AccountRequest req = a_request("maria@example.org");

  setup();
  assert(account_register_provider(MOD_A, &provider));

  assert(account_verify(CLI_A, &req, on_done, 0) != 0);
  assert(account_lookup(CLI_B, "maria", on_owner, 0) != 0);
  assert(account_pending_count() == 2);

  account_unregister_provider(MOD_A);

  assert(account_pending_count() == 0);
  assert(!account_have_provider());

  /* Both kinds are told, each in its own shape: a caller left waiting on
   * a module that has gone would wait for ever. */
  assert(done_calls == 1);
  assert(done_result == ACCOUNT_ERR_UNAVAILABLE);
  assert(owner_calls == 1);
  assert(owner_result == ACCOUNT_NICK_UNKNOWN);

  printf("ok - withdrawing a provider fails what it still owed\n");
}

/** Every result has a distinct, non-empty explanation. */
static void test_strerror(void)
{
  int i;
  int j;

  for (i = 0; i < ACCOUNT_ERR_LAST; i++) {
    const char* text = account_strerror((enum AccountResult) i);

    assert(text && *text);

    for (j = 0; j < i; j++)
      assert(strcmp(text, account_strerror((enum AccountResult) j)) != 0);
  }

  assert(0 == strcmp(account_strerror((enum AccountResult) 99),
                     "unknown error"));

  printf("ok - every result explains itself\n");
}

/** Guest names are distinct, well-formed, and fit in a nickname. */
static void test_guest_nick(void)
{
  char seen[64][NICKLEN + 2];
  char nick[NICKLEN + 2];
  char small[8];
  int i;
  int j;

  setup();

  for (i = 0; i < 64; i++) {
    assert(account_guest_nick(nick, sizeof(nick)));
    assert(strlen(nick) <= NICKLEN);
    assert(0 == strncmp(nick, "guest-", 6));

    /* Different every time: a guest name that repeats is two strangers
     * with one nickname. */
    for (j = 0; j < i; j++)
      assert(strcmp(nick, seen[j]) != 0);

    strcpy(seen[i], nick);
  }

  /* A buffer that cannot hold prefix plus random part is refused rather
   * than filled with a shorter, more collidable name. */
  assert(!account_guest_nick(small, sizeof(small)));

  /* And so is a configured prefix that leaves no room: a prefix long
   * enough to squeeze out the random part would make every guest the
   * same guest. */
  stub_guest_prefix = "averyveryverylongprefix-";
  assert(!account_guest_nick(nick, sizeof(nick)));

  /* An empty one falls back rather than producing a bare random name. */
  stub_guest_prefix = "";
  assert(account_guest_nick(nick, sizeof(nick)));
  assert(0 == strncmp(nick, "guest-", 6));
  stub_guest_prefix = "guest-";

  printf("ok - guest nicknames are distinct and fit\n");
}

/** A listing is asked of the address and answered on its own. */
static void test_list(void)
{
  static const struct AccountEntry entries[] = {
    { "maria", 0 },
    { "maria_movil", 0 },
    { "mrodriguez", 1 }
  };
  account_id_t id;

  setup();
  assert(account_register_provider(MOD_A, &provider));

  assert(account_list(CLI_A, "", on_list, 0) == 0);
  assert(account_list(CLI_A, 0, on_list, 0) == 0);
  assert(prov_list_calls == 0);

  id = account_list(CLI_A, "maria@example.org", on_list, 0);
  assert(id != 0);
  assert(prov_list_calls == 1);
  assert(0 == strcmp(prov_last_email, "maria@example.org"));

  /* The three kinds of answer do not cross. */
  assert(!account_complete(id, ACCOUNT_OK, "maria", 0, 0));
  assert(!account_complete_owner(id, ACCOUNT_NICK_FREE));
  assert(done_calls == 0 && owner_calls == 0);
  assert(account_pending_count() == 1);

  assert(account_complete_list(id, ACCOUNT_OK, entries, 3, 0));
  assert(list_calls == 1);
  assert(list_result == ACCOUNT_OK);
  assert(list_client == CLI_A);
  assert(list_count == 3);
  assert(0 == strcmp(list_first, "maria"));
  assert(!list_first_default);
  assert(account_pending_count() == 0);

  /* And the handle is spent. */
  assert(!account_complete_list(id, ACCOUNT_OK, entries, 3, 0));
  assert(list_calls == 1);

  printf("ok - a listing is answered on its own\n");
}

/** A listing that fails reaches the caller with no entries. */
static void test_list_fails(void)
{
  account_id_t id;

  setup();
  assert(account_register_provider(MOD_A, &provider));

  id = account_list(CLI_A, "maria@example.org", on_list, 0);
  assert(id != 0);

  assert(account_complete_list(id, ACCOUNT_ERR_UNAVAILABLE, 0, 0, 0));
  assert(list_calls == 1);
  assert(list_result == ACCOUNT_ERR_UNAVAILABLE);
  assert(list_count == 0);

  /* So does a provider that goes away owing one, and a deadline. */
  done_reset();
  id = account_list(CLI_A, "maria@example.org", on_list, 0);
  assert(id != 0);
  account_unregister_provider(MOD_A);
  assert(list_calls == 1);
  assert(list_result == ACCOUNT_ERR_UNAVAILABLE);
  assert(account_pending_count() == 0);

  done_reset();
  assert(account_register_provider(MOD_A, &provider));
  id = account_list(CLI_A, "maria@example.org", on_list, 0);
  assert(id != 0);
  assert(account_expire(CurrentTime + 3600) == 1);
  assert(list_calls == 1);
  assert(list_result == ACCOUNT_ERR_TIMEOUT);

  printf("ok - a listing that fails carries no entries\n");
}

int main(void)
{
  test_register();
  test_no_provider();
  test_verify();
  test_verify_answers_immediately();
  test_lookup();
  test_list();
  test_list_fails();
  test_client_gone();
  test_expire();
  test_provider_withdrawn();
  test_strerror();
  test_guest_nick();

  account_close();

  printf("ok - account_t\n");
  return 0;
}
