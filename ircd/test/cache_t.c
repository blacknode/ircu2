/* cache_t.c - Test the cache driver register and the calls in flight.
 *
 * The cache is never the truth: every caller reads it first and the
 * database second, so the thing this has to pin down is that a cache
 * which is missing, empty, slow or broken costs a query and nothing else.
 * A missing driver in particular must not be an error anybody has to
 * handle: cache_get() returns zero, nothing calls back, and the caller
 * goes to the database exactly as it would on a miss.
 */

#include "cache.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_string.h"

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

/* Two module handles; the register only compares these pointers. */
static struct ModuleHandle* const MOD_A = (struct ModuleHandle*) 0x1;
static struct ModuleHandle* const MOD_B = (struct ModuleHandle*) 0x2;

/* --- what the fake driver was asked ---------------------------------- */

static int drv_get_calls;
static int drv_set_calls;
static int drv_del_calls;
static int drv_cancel_calls;
static cache_id_t drv_last_id;
static char drv_last_key[CACHE_KEY_MAX + 1];
static char drv_last_value[256];
static size_t drv_last_len;
static int drv_last_ttl;
/** What the driver returns from its entry points. */
static enum CacheError drv_accept = CACHE_OK;
/** When non-zero, the driver answers from inside the ask. */
static int drv_answer_now;

static void drv_reset(void)
{
  drv_get_calls = drv_set_calls = drv_del_calls = drv_cancel_calls = 0;
  drv_last_id = 0;
  drv_last_key[0] = drv_last_value[0] = '\0';
  drv_last_len = 0;
  drv_last_ttl = 0;
  drv_accept = CACHE_OK;
  drv_answer_now = 0;
}

static enum CacheError drv_get(cache_id_t id, const char* key)
{
  drv_get_calls++;
  drv_last_id = id;
  ircd_strncpy(drv_last_key, key, CACHE_KEY_MAX);

  if (drv_accept != CACHE_OK)
    return drv_accept;
  if (drv_answer_now)
    cache_complete(id, "cached", 6, CACHE_OK, 0);

  return CACHE_OK;
}

static enum CacheError drv_set(cache_id_t id, const char* key,
                               const char* value, size_t len, int ttl)
{
  drv_set_calls++;
  drv_last_id = id;
  drv_last_ttl = ttl;
  drv_last_len = len;
  ircd_strncpy(drv_last_key, key, CACHE_KEY_MAX);
  if (len < sizeof(drv_last_value)) {
    memcpy(drv_last_value, value, len);
    drv_last_value[len] = '\0';
  }

  return drv_accept;
}

static enum CacheError drv_del(cache_id_t id, const char* key)
{
  drv_del_calls++;
  drv_last_id = id;
  ircd_strncpy(drv_last_key, key, CACHE_KEY_MAX);

  return drv_accept;
}

static void drv_cancel(cache_id_t id)
{
  drv_cancel_calls++;
  (void) id;
}

static const struct CacheDriver driver = {
  "test", drv_get, drv_set, drv_del, drv_cancel
};

static const struct CacheDriver driver_two = {
  "other", drv_get, drv_set, drv_del, drv_cancel
};

static const struct CacheDriver driver_broken = {
  "broken", 0, drv_set, drv_del, drv_cancel
};

/* --- what the caller was told ---------------------------------------- */

static int res_calls;
static enum CacheError res_code;
static int res_hit;
static char res_key[CACHE_KEY_MAX + 1];
static char res_value[256];
static void* res_user;

static void res_reset(void)
{
  res_calls = 0;
  res_code = CACHE_OK;
  res_hit = 0;
  res_key[0] = res_value[0] = '\0';
  res_user = 0;
}

static void on_result(const struct CacheResult* res, void* user)
{
  res_calls++;
  res_code = res->cres_code;
  res_hit = res->cres_hit;
  res_user = user;
  ircd_strncpy(res_key, res->cres_key ? res->cres_key : "", CACHE_KEY_MAX);

  if (res->cres_value && res->cres_len < sizeof(res_value)) {
    memcpy(res_value, res->cres_value, res->cres_len);
    res_value[res->cres_len] = '\0';
  }
}

/** A Redis{} block, so cache_available() can be true. */
static void configure(const char* prefix)
{
  const char* err = 0;
  char* host;
  char* pfx = 0;

  DupString(host, "127.0.0.1");
  cache_conf_clear();
  cache_conf_set_host(host);
  if (prefix) {
    DupString(pfx, prefix);
    cache_conf_set_prefix(pfx);
  }
  assert(cache_conf_commit(&err));
}

static void setup(void)
{
  cache_shutdown();
  drv_reset();
  res_reset();
  configure("ircu:");
}

/** One driver at a time, and it must be able to do all three things. */
static void test_register(void)
{
  setup();

  assert(!cache_available());
  assert(0 == strcmp(cache_driver_name(), "none"));

  assert(!cache_register_driver(MOD_A, &driver_broken));
  assert(!cache_register_driver(MOD_A, 0));
  assert(!cache_available());

  assert(cache_register_driver(MOD_A, &driver));
  assert(cache_available());
  assert(0 == strcmp(cache_driver_name(), "test"));

  /* A second is refused rather than replacing the first. */
  assert(!cache_register_driver(MOD_B, &driver_two));
  assert(0 == strcmp(cache_driver_name(), "test"));

  /* Only whoever registered it withdraws it. */
  cache_unregister_driver(MOD_B);
  assert(cache_available());

  cache_unregister_driver(MOD_A);
  assert(!cache_available());

  printf("ok - one cache driver at a time\n");
}

/** With no driver, nothing is asked and nothing calls back.
 *
 * This is the property the whole design leans on: a caller does not have
 * to know whether there is a cache.
 */
static void test_no_driver(void)
{
  setup();

  assert(cache_get(0, "nick:maria", on_result, 0) == 0);
  assert(cache_set(0, "nick:maria", "{}", 0, 60, on_result, 0) == 0);
  assert(cache_del(0, "nick:maria", on_result, 0) == 0);

  assert(res_calls == 0);
  assert(cache_calls_pending() == 0);

  printf("ok - with no driver nothing is asked and nothing calls back\n");
}

/** Nor with a driver but no Redis{} block. */
static void test_no_config(void)
{
  cache_shutdown();
  drv_reset();
  res_reset();

  assert(cache_register_driver(MOD_A, &driver));
  assert(!cache_available() && "a driver with nowhere to connect");
  assert(cache_get(0, "nick:maria", on_result, 0) == 0);
  assert(drv_get_calls == 0);

  printf("ok - a driver without a Redis{} block is not asked\n");
}

/** A hit, a miss, and the prefix. */
static void test_get(void)
{
  cache_id_t id;

  setup();
  assert(cache_register_driver(MOD_A, &driver));

  id = cache_get(0, "nick:maria", on_result, (void*) 0x55);
  assert(id != 0);
  assert(drv_get_calls == 1);

  /* The prefix is applied by the core, once, so a driver cannot forget
   * it and two networks cannot read each other's keys. */
  assert(0 == strcmp(drv_last_key, "ircu:nick:maria"));
  assert(res_calls == 0);
  assert(cache_calls_pending() == 1);

  assert(cache_complete(id, "{\"id\":1}", 8, CACHE_OK, 0));
  assert(res_calls == 1);
  assert(res_code == CACHE_OK);
  assert(res_hit == 1);
  assert(res_user == (void*) 0x55);
  assert(0 == strcmp(res_value, "{\"id\":1}"));
  assert(0 == strcmp(res_key, "ircu:nick:maria"));
  assert(cache_calls_pending() == 0);

  /* Answering twice finds nothing. */
  assert(!cache_complete(id, "x", 1, CACHE_OK, 0));

  /* A miss is CACHE_OK with no value: not finding something is not a
   * failure, it is the answer. */
  res_reset();
  id = cache_get(0, "nick:nobody", on_result, 0);
  assert(cache_complete(id, 0, 0, CACHE_OK, 0));
  assert(res_calls == 1);
  assert(res_code == CACHE_OK);
  assert(res_hit == 0);

  printf("ok - a hit, a miss, and the prefix the core applies\n");
}

/** A driver with the answer to hand may give it before the ask returns. */
static void test_answer_immediately(void)
{
  cache_id_t id;

  setup();
  assert(cache_register_driver(MOD_A, &driver));
  drv_answer_now = 1;

  id = cache_get(0, "nick:maria", on_result, 0);

  assert(id != 0);
  assert(res_calls == 1);
  assert(res_hit == 1);
  assert(cache_calls_pending() == 0);

  printf("ok - a driver may answer from inside the ask\n");
}

/** Writing: the default expiry, the ceiling, and what is too big. */
static void test_set(void)
{
  static char big[CACHE_VALUE_MAX + 2];
  cache_id_t id;

  setup();
  assert(cache_register_driver(MOD_A, &driver));

  id = cache_set(0, "nick:maria", "{\"id\":1}", 0, 0, on_result, 0);
  assert(id != 0);
  assert(drv_set_calls == 1);
  assert(drv_last_len == 8 && "a length of zero means strlen()");
  assert(drv_last_ttl == CACHE_TTL_DEFAULT);
  assert(0 == strcmp(drv_last_value, "{\"id\":1}"));
  assert(cache_complete(id, 0, 0, CACHE_OK, 0));

  /* An entry that outlives the server that wrote it is one nobody
   * remembers the reason for. */
  id = cache_set(0, "k", "v", 0, CACHE_TTL_MAX * 10, 0, 0);
  assert(drv_last_ttl == CACHE_TTL_MAX);
  cache_complete(id, 0, 0, CACHE_OK, 0);

  /* Bigger than the limit belongs in the database, not here.  Refused
   * before the driver sees it, and without a callback: there is nothing
   * to report to a caller that has not been promised anything. */
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  res_reset();
  assert(cache_set(0, "k", big, 0, 60, on_result, 0) == 0);
  assert(drv_set_calls == 2 && "the driver was not asked");
  assert(res_calls == 0);

  printf("ok - writing clamps the expiry and refuses what is too big\n");
}

/** Deleting is what a writer calls after it has written. */
static void test_del(void)
{
  cache_id_t id;

  setup();
  assert(cache_register_driver(MOD_A, &driver));

  id = cache_del(0, "nick:maria", on_result, 0);
  assert(id != 0);
  assert(drv_del_calls == 1);
  assert(0 == strcmp(drv_last_key, "ircu:nick:maria"));
  assert(cache_complete(id, 0, 0, CACHE_OK, 0));
  assert(res_calls == 1);
  assert(res_code == CACHE_OK);

  printf("ok - a key can be deleted\n");
}

/** A driver that refuses outright still answers exactly once. */
static void test_driver_refuses(void)
{
  setup();
  assert(cache_register_driver(MOD_A, &driver));
  drv_accept = CACHE_ERR_CONNECT;

  assert(cache_get(0, "k", on_result, 0) == 0);
  assert(res_calls == 1);
  assert(res_code == CACHE_ERR_CONNECT);
  assert(cache_calls_pending() == 0);

  printf("ok - a refusal is an answer, delivered once\n");
}

/** A key longer than the limit, prefix included, is refused. */
static void test_key_too_long(void)
{
  char key[CACHE_KEY_MAX + 8];

  setup();
  assert(cache_register_driver(MOD_A, &driver));

  memset(key, 'k', sizeof(key) - 1);
  key[sizeof(key) - 1] = '\0';

  assert(cache_get(0, key, on_result, 0) == 0);
  assert(drv_get_calls == 0);
  assert(cache_calls_pending() == 0);

  printf("ok - an oversized key never reaches the driver\n");
}

/** A deadline that passes is a failure, not a silence. */
static void test_expire(void)
{
  cache_id_t id;

  setup();
  assert(cache_register_driver(MOD_A, &driver));

  CurrentTime = 5000;
  id = cache_get(0, "k", on_result, 0);
  assert(id != 0);

  assert(cache_expire(CurrentTime) == 0);
  assert(res_calls == 0);

  assert(cache_expire(CurrentTime + 60) == 1);
  assert(res_calls == 1);
  assert(res_code == CACHE_ERR_TIMEOUT);
  assert(res_hit == 0);
  assert(drv_cancel_calls == 1);
  assert(cache_calls_pending() == 0);

  assert(!cache_complete(id, "late", 4, CACHE_OK, 0));

  printf("ok - a call that is never answered times out\n");
}

/** Withdrawing the driver fails everything it still owed. */
static void test_driver_withdrawn(void)
{
  setup();
  assert(cache_register_driver(MOD_A, &driver));

  assert(cache_get(0, "a", on_result, 0) != 0);
  assert(cache_get(0, "b", on_result, 0) != 0);
  assert(cache_calls_pending() == 2);

  cache_unregister_driver(MOD_A);

  assert(cache_calls_pending() == 0);
  assert(res_calls == 2);
  assert(res_code == CACHE_ERR_UNAVAILABLE);
  assert(!cache_available());

  printf("ok - withdrawing a driver fails what it still owed\n");
}

/** Unloading a module drops what it asked, whether or not it is the
 * driver. */
static void test_drop_module(void)
{
  setup();
  assert(cache_register_driver(MOD_A, &driver));

  assert(cache_get(MOD_B, "b", on_result, 0) != 0);
  assert(cache_get(0, "core", on_result, 0) != 0);
  assert(cache_calls_pending() == 2);

  cache_drop_module(MOD_B);

  /* Dropped without an answer: there is nobody left to answer to, and the
   * callback is in code about to be unmapped. */
  assert(cache_calls_pending() == 1);
  assert(res_calls == 0);
  assert(drv_cancel_calls == 1);

  /* And the driver's own module takes the register with it. */
  cache_drop_module(MOD_A);
  assert(!cache_available());
  assert(cache_calls_pending() == 0);

  printf("ok - unloading a module drops what it asked\n");
}

/** The Redis{} block: what it needs and what it corrects. */
static void test_config(void)
{
  const char* err = 0;
  char* s;

  cache_shutdown();

  /* Somewhere to connect is the one thing with no default. */
  cache_conf_clear();
  assert(!cache_conf_commit(&err));
  assert(err && *err);

  /* A socket counts as somewhere. */
  cache_conf_clear();
  DupString(s, "/run/redis.sock");
  cache_conf_set_socket(s);
  assert(cache_conf_commit(&err));
  assert(cache_conf() != 0);
  assert(0 == strcmp(cache_conf()->cconf_socket, "/run/redis.sock"));
  assert(cache_conf()->cconf_port == 6379 && "a default port");

  /* Nonsense is corrected rather than refused: a server should not fail
   * to start over a cache setting. */
  cache_conf_clear();
  DupString(s, "127.0.0.1");
  cache_conf_set_host(s);
  cache_conf_set_port(99999);
  cache_conf_set_pool(0);
  cache_conf_set_timeout(CACHE_TIMEOUT_MAX_MS * 100);
  assert(cache_conf_commit(&err));
  assert(cache_conf()->cconf_port == 6379);
  assert(cache_conf()->cconf_pool == 1);
  assert(cache_conf()->cconf_timeout_ms == CACHE_TIMEOUT_MAX_MS);

  /* Taking the block out of ircd.conf stops the cache being used. */
  assert(cache_register_driver(MOD_A, &driver));
  assert(cache_available());
  cache_conf_unmark();
  cache_conf_sweep();
  assert(cache_conf() == 0);
  assert(!cache_available() && "no block, no cache");
  assert(0 == strcmp(cache_driver_name(), "test") && "the driver stays");

  printf("ok - the Redis block validates and corrects\n");
}

/** Counters, because /STATS will show them. */
static void test_counters(void)
{
  cache_id_t id;

  setup();
  assert(cache_register_driver(MOD_A, &driver));

  id = cache_get(0, "a", on_result, 0);
  cache_complete(id, "v", 1, CACHE_OK, 0);
  id = cache_get(0, "b", on_result, 0);
  cache_complete(id, 0, 0, CACHE_OK, 0);
  id = cache_get(0, "c", on_result, 0);
  cache_complete(id, 0, 0, CACHE_ERR_BACKEND, 0);

  assert(cache_calls_total() == 3);
  assert(cache_calls_hit() == 1);
  /* A miss is not a failure. */
  assert(cache_calls_failed() == 1);
  assert(cache_calls_pending() == 0);

  printf("ok - a miss counts as neither a hit nor a failure\n");
}

/** Every error has a distinct, non-empty explanation. */
static void test_strerror(void)
{
  int i;
  int j;

  for (i = 0; i < CACHE_ERR_LAST; i++) {
    const char* text = cache_strerror((enum CacheError) i);

    assert(text && *text);

    for (j = 0; j < i; j++)
      assert(strcmp(text, cache_strerror((enum CacheError) j)) != 0);
  }

  assert(0 == strcmp(cache_strerror((enum CacheError) 99),
                     "unknown cache error"));

  printf("ok - every error explains itself\n");
}

int main(void)
{
  test_register();
  test_no_driver();
  test_no_config();
  test_get();
  test_answer_immediately();
  test_set();
  test_del();
  test_driver_refuses();
  test_key_too_long();
  test_expire();
  test_driver_withdrawn();
  test_drop_module();
  test_config();
  test_counters();
  test_strerror();

  cache_shutdown();

  printf("ok - cache_t\n");
  return 0;
}
