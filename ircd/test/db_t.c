/* db_t.c - Test the database facade.
 *
 * db.c has no database in it, so neither does this: a fake driver stands in
 * for the postgres module, and the test checks the things the facade is
 * actually responsible for.
 *
 * Covers: the Database{} block is validated and its timeout clamped to five
 * seconds however the configuration spells it; a role falls back to the
 * common DSN; a pass with no block drops the old one; there is exactly one
 * driver at a time; a query reaches the driver and its answer reaches the
 * callback; a driver that refuses a query does not leave a call behind;
 * unloading the calling module drops its callback without dropping
 * anybody else's; and withdrawing the driver fails whatever is left.
 */

#include "db.h"
#include "ircd_log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Stub for the one thing db.c uses out of module.c. */
const char* module_name(const struct ModuleHandle* mod)
{
  (void) mod;
  return "fake";
}

/* Two module handles are enough to test per-module teardown; db.c only ever
 * compares these pointers.
 */
static struct ModuleHandle* const MOD_A = (struct ModuleHandle*) 0x1;
static struct ModuleHandle* const MOD_B = (struct ModuleHandle*) 0x2;

/* ------------------------------------------------------------------------
 * A driver that does nothing but remember what it was asked.
 * ------------------------------------------------------------------------ */

static unsigned long fake_last_id;      /**< Id of the last submit. */
static enum DbRole fake_last_role;      /**< Role of the last submit. */
static char fake_last_sql[256];         /**< SQL of the last submit. */
static unsigned int fake_last_params;   /**< Parameters it carried. */
static unsigned int fake_submits;       /**< Submits seen. */
static unsigned int fake_releases;      /**< Results released. */
static enum DbError fake_answer = DB_OK; /**< What submit should return. */

static enum DbError fake_submit(unsigned long id, const struct DbQuery* query,
                                enum DbRole role)
{
  unsigned int n = 0;

  if (fake_answer != DB_OK)
    return fake_answer;

  fake_submits++;
  fake_last_id = id;
  fake_last_role = role;

  strncpy(fake_last_sql, query->sql, sizeof(fake_last_sql) - 1);
  fake_last_sql[sizeof(fake_last_sql) - 1] = '\0';

  if (query->params)
    while (query->params[n])
      n++;
  fake_last_params = n;

  return DB_OK;
}

static void fake_release(struct json_t* data)
{
  (void) data;
  fake_releases++;
}

static const struct DbDriver fake_driver = {
  "fake",
  fake_submit,
  fake_release
};

/* A second driver, to check that one is the limit. */
static const struct DbDriver other_driver = {
  "other",
  fake_submit,
  fake_release
};

/* ------------------------------------------------------------------------
 * A consumer that records what it was told.
 * ------------------------------------------------------------------------ */

static unsigned int seen_calls;         /**< Callbacks that ran. */
static enum DbError seen_code;          /**< Code of the last one. */
static char seen_message[DB_ERRMSG_LEN + 1]; /**< Its message. */
static unsigned int seen_rows;          /**< Its row count. */
static void* seen_user;                 /**< Its user pointer. */

static void on_result(const struct DbResult* res, void* user)
{
  seen_calls++;
  seen_code = res->err.dberr_code;
  seen_rows = res->rows;
  seen_user = user;
  strcpy(seen_message, res->err.dberr_message);
}

static void reset_seen(void)
{
  seen_calls = 0;
  seen_code = DB_OK;
  seen_rows = 0;
  seen_user = 0;
  seen_message[0] = '\0';
}

/** Configure a Database{} block the way the parser would. */
static int configure(const char* dsn, const char* read_dsn, int pool,
                     int timeout_ms, const char** err)
{
  db_conf_unmark();
  db_conf_clear();

  if (dsn)
    db_conf_set_dsn(-1, strdup(dsn));
  if (read_dsn)
    db_conf_set_dsn(DB_ROLE_READ, strdup(read_dsn));
  if (pool)
    db_conf_set_pool(-1, pool);
  if (timeout_ms)
    db_conf_set_timeout(timeout_ms);

  return db_conf_commit(err);
}

/* ------------------------------------------------------------------------
 * The tests.
 * ------------------------------------------------------------------------ */

static void test_conf_requires_dsn(void)
{
  const char* err = 0;

  db_conf_unmark();
  db_conf_clear();
  db_conf_set_pool(-1, 4);

  assert(!db_conf_commit(&err));
  assert(err != 0);
  assert(!db_conf());

  printf("ok - a Database block with no dsn is refused\n");
}

static void test_conf_defaults_and_clamp(void)
{
  const char* err = 0;

  assert(configure("host=primary", 0, 0, 0, &err));
  assert(db_conf() != 0);
  assert(!strcmp(db_conf_dsn(DB_ROLE_READ), "host=primary"));
  assert(!strcmp(db_conf_dsn(DB_ROLE_WRITE), "host=primary"));
  assert(db_conf_pool(DB_ROLE_READ) == DB_DEFAULT_POOL);
  assert(db_conf_timeout() == DB_TIMEOUT_DEFAULT_MS);

  printf("ok - a block with only a dsn gets the defaults\n");

  /* Both roles resolve to the replica only where one was named. */
  assert(configure("host=primary", "host=replica", 8, 1500, &err));
  assert(!strcmp(db_conf_dsn(DB_ROLE_READ), "host=replica"));
  assert(!strcmp(db_conf_dsn(DB_ROLE_WRITE), "host=primary"));
  assert(db_conf_pool(DB_ROLE_READ) == 8);
  assert(db_conf_pool(DB_ROLE_WRITE) == 8);
  assert(db_conf_timeout() == 1500);

  printf("ok - read falls back to dsn only when it is not set\n");

  /* The ceiling is not negotiable, and the block is still accepted. */
  assert(configure("host=primary", 0, 0, 60000, &err));
  assert(db_conf_timeout() == DB_TIMEOUT_MAX_MS);

  assert(configure("host=primary", 0, 0, -5, &err));
  assert(db_conf_timeout() == DB_TIMEOUT_DEFAULT_MS);

  printf("ok - the timeout is clamped to %dms\n", DB_TIMEOUT_MAX_MS);

  /* An absurd pool is corrected rather than obeyed. */
  assert(configure("host=primary", 0, DB_MAX_POOL * 4, 0, &err));
  assert(db_conf_pool(DB_ROLE_READ) == DB_MAX_POOL);

  printf("ok - the pool size is clamped to %d\n", DB_MAX_POOL);
}

static void test_conf_generation_and_sweep(void)
{
  const char* err = 0;
  unsigned int first;

  assert(configure("host=primary", 0, 0, 0, &err));
  first = db_conf_generation();
  assert(first != 0);

  assert(configure("host=other", 0, 0, 0, &err));
  assert(db_conf_generation() != first);

  printf("ok - the generation moves when the configuration does\n");

  /* A pass that mentions no Database{} block takes the old one away. */
  db_conf_unmark();
  db_conf_sweep();
  assert(!db_conf());
  assert(!db_conf_dsn(DB_ROLE_READ));
  assert(db_conf_generation() == 0);

  printf("ok - a pass with no block drops the configuration\n");
}

static void test_one_driver(void)
{
  const char* err = 0;

  assert(configure("host=primary", 0, 0, 0, &err));

  assert(!db_available());
  assert(!db_driver_name());

  assert(db_register_driver(MOD_A, &fake_driver));
  assert(db_available());
  assert(!strcmp(db_driver_name(), "fake"));

  /* A second one is refused rather than quietly replacing the first. */
  assert(!db_register_driver(MOD_B, &other_driver));
  assert(!strcmp(db_driver_name(), "fake"));

  printf("ok - there is one driver at a time\n");
}

static void test_query_round_trip(void)
{
  struct DbParam nick = { DB_TYPE_TEXT, "somebody", DB_FORMAT_TEXT };
  struct DbParam id = { DB_TYPE_INT, "42", DB_FORMAT_TEXT };
  struct DbParam* params[] = { &nick, &id, 0 };
  struct DbQuery query;
  int marker = 0;

  reset_seen();
  fake_submits = 0;
  fake_releases = 0;

  query.sql = "select * from accounts where nick = $1 and id = $2";
  query.params = params;

  assert(db_query(MOD_A, &query, on_result, &marker) == DB_OK);
  assert(fake_submits == 1);
  assert(fake_last_role == DB_ROLE_READ);
  assert(fake_last_params == 2);
  assert(!strcmp(fake_last_sql, query.sql));
  assert(db_calls_pending() == 1);
  assert(seen_calls == 0);          /* nothing is synchronous */

  /* The driver answers, in the main thread, later. */
  db_complete(fake_last_id, 0, 3, DB_OK, 0);

  assert(seen_calls == 1);
  assert(seen_code == DB_OK);
  assert(seen_rows == 3);
  assert(seen_user == &marker);
  assert(db_calls_pending() == 0);

  printf("ok - a query reaches the driver and its answer the caller\n");

  /* db_exec() is the same thing on the other role. */
  query.params = 0;
  assert(db_exec(MOD_A, &query, on_result, &marker) == DB_OK);
  assert(fake_last_role == DB_ROLE_WRITE);
  assert(fake_last_params == 0);
  db_complete(fake_last_id, 0, 0, DB_OK, 0);

  printf("ok - db_exec routes to the write role\n");
}

static void test_error_message(void)
{
  struct DbQuery query = { "select 1", 0 };

  reset_seen();

  assert(db_query(MOD_A, &query, on_result, 0) == DB_OK);
  db_complete(fake_last_id, 0, 0, DB_ERR_UNIQUE, "that name is taken");

  assert(seen_calls == 1);
  assert(seen_code == DB_ERR_UNIQUE);
  assert(!strcmp(seen_message, "that name is taken"));

  /* No message from the driver means the facade supplies one. */
  assert(db_query(MOD_A, &query, on_result, 0) == DB_OK);
  db_complete(fake_last_id, 0, 0, DB_ERR_TIMEOUT, 0);

  assert(seen_code == DB_ERR_TIMEOUT);
  assert(!strcmp(seen_message, db_strerror(DB_ERR_TIMEOUT)));

  printf("ok - the error code and message reach the caller\n");
}

static void test_refusals(void)
{
  struct DbQuery bad = { 0, 0 };
  struct DbQuery empty = { "", 0 };
  struct DbQuery query = { "select 1", 0 };

  reset_seen();

  assert(db_query(MOD_A, &bad, on_result, 0) == DB_ERR_PARAM);
  assert(db_query(MOD_A, &empty, on_result, 0) == DB_ERR_PARAM);
  assert(db_calls_pending() == 0);
  assert(seen_calls == 0);

  printf("ok - a query with no statement is refused outright\n");

  /* A driver that refuses must not leave a call waiting for an answer it
   * has already decided not to give.
   */
  fake_answer = DB_ERR_BUSY;
  assert(db_query(MOD_A, &query, on_result, 0) == DB_ERR_BUSY);
  assert(db_calls_pending() == 0);
  assert(seen_calls == 0);
  fake_answer = DB_OK;

  printf("ok - a refused submit leaves no call behind\n");
}

static void test_module_unload(void)
{
  struct DbQuery query = { "select 1", 0 };
  unsigned long a_id;

  reset_seen();
  fake_releases = 0;

  assert(db_query(MOD_B, &query, on_result, 0) == DB_OK);
  a_id = fake_last_id;
  assert(db_query(MOD_A, &query, on_result, 0) == DB_OK);
  assert(db_calls_pending() == 2);

  /* MOD_B goes away.  Its callback must not run -- that is the whole point
   * of the facade -- but MOD_A's must still work.
   */
  db_drop_module(MOD_B);
  assert(db_calls_pending() == 1);

  db_complete(a_id, (struct json_t*) 0x1234, 0, DB_OK, 0);
  assert(seen_calls == 0);          /* the module that asked is gone */
  assert(fake_releases == 1);       /* but the result was still released */

  db_complete(fake_last_id, 0, 1, DB_OK, 0);
  assert(seen_calls == 1);
  assert(db_calls_pending() == 0);

  printf("ok - unloading a module drops only its own callbacks\n");
}

static void test_driver_withdrawn(void)
{
  struct DbQuery query = { "select 1", 0 };

  reset_seen();

  assert(db_query(MOD_A, &query, on_result, 0) == DB_OK);
  assert(db_calls_pending() == 1);

  /* Only the module that registered it may withdraw it. */
  db_unregister_driver(MOD_B);
  assert(db_available());
  assert(db_calls_pending() == 1);

  db_unregister_driver(MOD_A);

  assert(!db_available());
  assert(!db_driver_name());
  assert(db_calls_pending() == 0);
  assert(seen_calls == 1);
  assert(seen_code == DB_ERR_UNAVAILABLE);

  /* With no driver there is nothing to submit to. */
  assert(db_query(MOD_A, &query, on_result, 0) == DB_ERR_UNAVAILABLE);

  printf("ok - withdrawing the driver fails whatever was in flight\n");
}

static void test_no_config(void)
{
  struct DbQuery query = { "select 1", 0 };

  db_conf_unmark();
  db_conf_sweep();
  assert(!db_conf());

  assert(db_register_driver(MOD_A, &fake_driver));
  assert(!db_available());          /* a driver with nothing to connect to */
  assert(db_query(MOD_A, &query, on_result, 0) == DB_ERR_CONFIG);

  db_unregister_driver(MOD_A);

  printf("ok - a driver with no configuration refuses queries\n");
}

int main(void)
{
  test_conf_requires_dsn();
  test_conf_defaults_and_clamp();
  test_conf_generation_and_sweep();
  test_one_driver();
  test_query_round_trip();
  test_error_message();
  test_refusals();
  test_module_unload();
  test_driver_withdrawn();
  test_no_config();

  db_shutdown();

  printf("db_t: all tests passed\n");

  return 0;
}
