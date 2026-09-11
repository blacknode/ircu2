/*
 * IRC - Internet Relay Chat, ircd/db.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/** @file
 * @brief The database facade: configuration, dispatch, and nothing else.
 *
 * There is no database code here.  This file holds the @c Database{} block,
 * the one driver a module may register, and the table of queries that are
 * waiting for an answer -- and it exists mostly so that the answer can be
 * thrown away safely when the module that asked for it is unloaded before
 * it arrives.
 *
 * A query in flight is a #DbCall with an identifier, and the driver is
 * handed the identifier rather than the callback.  That is the whole trick:
 * when a module goes away its calls are forgotten, and the driver's
 * db_complete() a second later finds nothing to call and drops the result
 * instead of jumping into an unmapped page.
 *
 * See include/db.h and doc/readme.database.
 */
#include "config.h"

#include "db.h"

#include "ircd_alloc.h"
#include "ircd_log.h"
#include "ircd_string.h"
#include "module.h"

#include <assert.h>
#include <string.h>

/** One query that has been accepted and not yet answered. */
struct DbCall {
  struct DbCall*       dbc_next;   /**< Next call in #db_calls. */
  unsigned long        dbc_id;     /**< Identifier the driver holds. */
  DbResultFn           dbc_fn;     /**< Callback, or NULL. */
  void*                dbc_user;   /**< Caller's opaque pointer. */
  struct ModuleHandle* dbc_owner;  /**< Module that asked, or NULL. */
};

/** The registered driver, or NULL. */
static const struct DbDriver* db_driver;
/** Module that registered #db_driver. */
static struct ModuleHandle* db_driver_owner;

/** Queries accepted and not yet answered. */
static struct DbCall* db_calls;
/** Identifier for the next call; never reused, never zero. */
static unsigned long db_next_id = 1;

/** The published @c Database{} block, or NULL if there is none. */
static struct DatabaseConf* db_config;
/** The block being read, filled in between db_conf_clear() and commit. */
static struct DatabaseConf db_pending;
/** Non-zero once db_conf_clear() has run in this configuration pass. */
static int db_pending_seen;

/** Statistics for db_calls_pending() and friends. */
static unsigned int db_stat_pending;
static unsigned int db_stat_total;   /**< @copydoc db_stat_pending */
static unsigned int db_stat_failed;  /**< @copydoc db_stat_pending */

/** Text for each #DbError, indexed by the enum. */
static const char* db_error_text[DB_ERR_LAST] = {
  "no error",
  "no database driver is loaded",
  "the Database{} block is missing or unusable",
  "could not connect to the database",
  "the database did not answer in time",
  "the database connection pool is busy",
  "the query or its parameters are malformed",
  "the database rejected the statement",
  "no such table, column or function",
  "the database refused the operation",
  "a unique constraint was violated",
  "a foreign key constraint was violated",
  "a value was null where none is allowed",
  "a constraint was violated",
  "a value was not acceptable to the database",
  "the connection is read-only",
  "the transaction was rolled back; retry it",
  "the database is out of resources",
  "the database driver failed"
};

const char* db_strerror(enum DbError code)
{
  unsigned int index = (unsigned int) code;

  if (index >= DB_ERR_LAST)
    return "unknown database error";

  return db_error_text[index];
}

/* ------------------------------------------------------------------------
 * The Database{} block.
 * ------------------------------------------------------------------------ */

/** Release every string in \a conf and zero it.
 * @param[in,out] conf Configuration to empty.
 */
static void db_conf_release(struct DatabaseConf* conf)
{
  int role;

  MyFree(conf->dbconf_dsn);
  for (role = 0; role < DB_ROLE_LAST; role++)
    MyFree(conf->dbconf_role_dsn[role]);

  memset(conf, 0, sizeof(*conf));
}

void db_conf_clear(void)
{
  db_conf_release(&db_pending);
  db_pending.dbconf_timeout_ms = DB_TIMEOUT_DEFAULT_MS;
  db_pending_seen = 1;
}

void db_conf_set_dsn(int role, char* dsn)
{
  assert(role >= -1 && role < DB_ROLE_LAST);

  if (role < 0) {
    MyFree(db_pending.dbconf_dsn);
    db_pending.dbconf_dsn = dsn;
  } else {
    MyFree(db_pending.dbconf_role_dsn[role]);
    db_pending.dbconf_role_dsn[role] = dsn;
  }
}

void db_conf_set_pool(int role, int size)
{
  assert(role >= -1 && role < DB_ROLE_LAST);

  if (role < 0) {
    for (role = 0; role < DB_ROLE_LAST; role++)
      db_pending.dbconf_role_pool[role] = size;
  } else
    db_pending.dbconf_role_pool[role] = size;
}

void db_conf_set_timeout(int ms)
{
  db_pending.dbconf_timeout_ms = ms;
}

int db_conf_commit(const char** errstr)
{
  static unsigned int generation;
  int role;

  assert(0 != errstr);

  /* The DSN is the one thing with no sensible default: a pool with nothing
   * to connect to is not a degraded configuration, it is a typo.
   */
  if (!db_pending.dbconf_dsn || !*db_pending.dbconf_dsn) {
    *errstr = "Database: dsn is required";
    db_conf_release(&db_pending);
    return 0;
  }

  /* Five seconds is the ceiling and it is not negotiable; see db.h.  A
   * configuration that asks for more is corrected rather than refused, so
   * that a server does not fail to start over it, but it is said out loud.
   */
  if (db_pending.dbconf_timeout_ms <= 0)
    db_pending.dbconf_timeout_ms = DB_TIMEOUT_DEFAULT_MS;
  if (db_pending.dbconf_timeout_ms > DB_TIMEOUT_MAX_MS) {
    log_write(LS_CONFIG, L_WARNING, 0,
              "Database: timeout of %dms exceeds the %dms maximum; using %dms",
              db_pending.dbconf_timeout_ms, DB_TIMEOUT_MAX_MS,
              DB_TIMEOUT_MAX_MS);
    db_pending.dbconf_timeout_ms = DB_TIMEOUT_MAX_MS;
  }

  for (role = 0; role < DB_ROLE_LAST; role++) {
    if (db_pending.dbconf_role_pool[role] <= 0)
      db_pending.dbconf_role_pool[role] = DB_DEFAULT_POOL;
    else if (db_pending.dbconf_role_pool[role] > DB_MAX_POOL) {
      log_write(LS_CONFIG, L_WARNING, 0,
                "Database: pool of %d exceeds the maximum of %d; using %d",
                db_pending.dbconf_role_pool[role], DB_MAX_POOL, DB_MAX_POOL);
      db_pending.dbconf_role_pool[role] = DB_MAX_POOL;
    }
  }

  if (!db_config)
    db_config = (struct DatabaseConf*) MyCalloc(1, sizeof(*db_config));
  else
    db_conf_release(db_config);

  *db_config = db_pending;
  memset(&db_pending, 0, sizeof(db_pending));

  db_config->dbconf_generation = ++generation;

  return 1;
}

void db_conf_unmark(void)
{
  db_pending_seen = 0;
}

void db_conf_sweep(void)
{
  /* The scratch block is finished with either way: a commit empties it, and
   * a block that never got as far as one -- a parse error in the middle of
   * it -- leaves it holding strings nobody will ever read.
   */
  db_conf_release(&db_pending);

  /* A configuration pass that mentioned no Database{} block leaves the
   * server with none: the block was removed, and a driver that kept using
   * the old DSN would be connecting somewhere the operator no longer lists.
   */
  if (db_pending_seen)
    return;

  if (db_config) {
    db_conf_release(db_config);
    MyFree(db_config);
    db_config = 0;
  }
}

const struct DatabaseConf* db_conf(void)
{
  return db_config;
}

const char* db_conf_dsn(enum DbRole role)
{
  assert(role >= 0 && role < DB_ROLE_LAST);

  if (!db_config)
    return 0;
  if (db_config->dbconf_role_dsn[role])
    return db_config->dbconf_role_dsn[role];

  return db_config->dbconf_dsn;
}

int db_conf_pool(enum DbRole role)
{
  assert(role >= 0 && role < DB_ROLE_LAST);

  return db_config ? db_config->dbconf_role_pool[role] : DB_DEFAULT_POOL;
}

int db_conf_timeout(void)
{
  int ms = db_config ? db_config->dbconf_timeout_ms : DB_TIMEOUT_DEFAULT_MS;

  /* Clamped on commit already; clamped again here because this is the value
   * a driver arms its deadline with, and the ceiling is worth more than the
   * one branch it costs.
   */
  return (ms > 0 && ms <= DB_TIMEOUT_MAX_MS) ? ms : DB_TIMEOUT_MAX_MS;
}

unsigned int db_conf_generation(void)
{
  return db_config ? db_config->dbconf_generation : 0;
}

/* ------------------------------------------------------------------------
 * Drivers.
 * ------------------------------------------------------------------------ */

int db_register_driver(struct ModuleHandle* mod, const struct DbDriver* driver)
{
  assert(0 != driver);

  if (!driver->dbdrv_name || !driver->dbdrv_submit || !driver->dbdrv_release) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Refusing an incomplete database driver from module %s",
              mod ? module_name(mod) : "the core");
    return 0;
  }

  if (db_driver) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Database driver %s is already loaded; refusing %s",
              db_driver->dbdrv_name, driver->dbdrv_name);
    return 0;
  }

  db_driver = driver;
  db_driver_owner = mod;

  log_write(LS_SYSTEM, L_INFO, 0, "Database driver %s registered",
            driver->dbdrv_name);

  return 1;
}

/** Take one call off #db_calls.
 * @param[in] id Identifier to look for.
 * @return The call, detached and still allocated, or NULL.
 */
static struct DbCall* db_call_take(unsigned long id)
{
  struct DbCall** call_p;
  struct DbCall* call;

  for (call_p = &db_calls; (call = *call_p); call_p = &call->dbc_next) {
    if (call->dbc_id == id) {
      *call_p = call->dbc_next;
      db_stat_pending--;
      return call;
    }
  }

  return 0;
}

/** Fail every call still waiting, with \a code.
 *
 * Used when the driver goes away.  The callbacks run here, in the main
 * thread, which is where they would have run anyway.
 * @param[in] code Error to report.
 */
static void db_fail_all(enum DbError code)
{
  struct DbResult res;
  struct DbCall* call;

  while ((call = db_calls)) {
    db_calls = call->dbc_next;
    db_stat_pending--;
    db_stat_failed++;

    if (call->dbc_fn) {
      memset(&res, 0, sizeof(res));
      res.err.dberr_code = code;
      ircd_strncpy(res.err.dberr_message, db_strerror(code), DB_ERRMSG_LEN);
      (*call->dbc_fn)(&res, call->dbc_user);
    }

    MyFree(call);
  }
}

void db_unregister_driver(struct ModuleHandle* mod)
{
  if (!db_driver || db_driver_owner != mod)
    return;

  db_driver = 0;
  db_driver_owner = 0;

  /* Nothing will ever answer these now. */
  db_fail_all(DB_ERR_UNAVAILABLE);
}

void db_drop_module(struct ModuleHandle* mod)
{
  struct DbCall** call_p;
  struct DbCall* call;

  assert(0 != mod);

  /* The module's own calls first: it is going away, so its callbacks are
   * dropped in silence rather than called.  The driver may still complete
   * them later; db_complete() will find nothing and release the result.
   */
  for (call_p = &db_calls; (call = *call_p);) {
    if (call->dbc_owner == mod) {
      *call_p = call->dbc_next;
      db_stat_pending--;
      MyFree(call);
    } else
      call_p = &call->dbc_next;
  }

  db_unregister_driver(mod);
}

void db_shutdown(void)
{
  db_fail_all(DB_ERR_UNAVAILABLE);

  db_driver = 0;
  db_driver_owner = 0;

  db_conf_release(&db_pending);
  if (db_config) {
    db_conf_release(db_config);
    MyFree(db_config);
    db_config = 0;
  }
}

/* ------------------------------------------------------------------------
 * Dispatch.
 * ------------------------------------------------------------------------ */

/** Reject \a query before it reaches the driver.
 * @param[in] query Query to check.
 * @return #DB_OK when it is worth submitting.
 */
static enum DbError db_check_query(const struct DbQuery* query)
{
  unsigned int n;

  if (!query || !query->sql || !*query->sql)
    return DB_ERR_PARAM;

  if (query->params) {
    for (n = 0; query->params[n]; n++)
      if (n >= DB_MAX_PARAMS)
        return DB_ERR_PARAM;
  }

  return DB_OK;
}

/** Submit \a query for \a role.
 * @param[in] mod Module making the call, or NULL for the core.
 * @param[in] query Statement and parameters.
 * @param[in] cb Callback, or NULL.
 * @param[in] user Opaque pointer for \a cb.
 * @param[in] role Which side to run it on.
 * @return #DB_OK when accepted.
 */
static enum DbError db_submit(struct ModuleHandle* mod,
                              const struct DbQuery* query,
                              DbResultFn cb, void* user, enum DbRole role)
{
  struct DbCall* call;
  enum DbError err;

  if ((err = db_check_query(query)) != DB_OK)
    return err;

  if (!db_driver)
    return DB_ERR_UNAVAILABLE;
  if (!db_config)
    return DB_ERR_CONFIG;

  call = (struct DbCall*) MyCalloc(1, sizeof(*call));
  call->dbc_id = db_next_id++;
  call->dbc_fn = cb;
  call->dbc_user = user;
  call->dbc_owner = mod;

  /* On the list before the driver is asked: a driver that manages to
   * complete the call from inside its own submit -- refusing it outright,
   * say -- must find it there.
   */
  call->dbc_next = db_calls;
  db_calls = call;
  db_stat_pending++;

  err = (*db_driver->dbdrv_submit)(call->dbc_id, query, role);

  if (err != DB_OK) {
    /* The driver refused; the call is ours to withdraw, unless the driver
     * already completed it on the way out.
     */
    if ((call = db_call_take(call->dbc_id)))
      MyFree(call);
    return err;
  }

  db_stat_total++;

  return DB_OK;
}

enum DbError db_query(struct ModuleHandle* mod, const struct DbQuery* query,
                      DbResultFn cb, void* user)
{
  return db_submit(mod, query, cb, user, DB_ROLE_READ);
}

enum DbError db_exec(struct ModuleHandle* mod, const struct DbQuery* query,
                     DbResultFn cb, void* user)
{
  return db_submit(mod, query, cb, user, DB_ROLE_WRITE);
}

void db_complete(unsigned long id, struct json_t* data, unsigned int rows,
                 enum DbError code, const char* message)
{
  struct DbResult res;
  struct DbCall* call = db_call_take(id);

  if (code != DB_OK)
    db_stat_failed++;

  if (call && call->dbc_fn) {
    memset(&res, 0, sizeof(res));
    res.data = data;
    res.rows = rows;
    res.err.dberr_code = code;
    ircd_strncpy(res.err.dberr_message,
                 message && *message ? message : db_strerror(code),
                 DB_ERRMSG_LEN);

    (*call->dbc_fn)(&res, call->dbc_user);
  }

  MyFree(call);

  /* Released even when nobody was left to look at it: the reference count
   * is the driver's, and leaking it because a module unloaded would be a
   * slow leak nobody notices.
   */
  if (data && db_driver)
    (*db_driver->dbdrv_release)(data);
}

int db_available(void)
{
  return db_driver && db_config;
}

const char* db_driver_name(void)
{
  return db_driver ? db_driver->dbdrv_name : 0;
}

unsigned int db_calls_pending(void)
{
  return db_stat_pending;
}

unsigned int db_calls_total(void)
{
  return db_stat_total;
}

unsigned int db_calls_failed(void)
{
  return db_stat_failed;
}
