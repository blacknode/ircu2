#ifndef INCLUDED_db_h
#define INCLUDED_db_h
/*
 * IRC - Internet Relay Chat, include/db.h
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
 * @brief Database access for modules, with no database library in sight.
 *
 * The server does not talk to a database.  It holds the @c Database{} block
 * from ircd.conf, it holds a table of the queries that are in flight, and it
 * holds one pointer to a @em driver -- a module that knows how to run a
 * query.  Everything that actually opens a socket to a database lives in
 * that module; today that is @c modules/workers/postgres/.
 *
 * Two things follow from that split, and both are the point of this file:
 *
 *   - @b No @b driver @b type @b escapes.  A consumer never sees a
 *     @c PGconn, a @c PGresult, an @c Oid or a libpq error code.  It sends a
 *     #DbQuery and receives a #DbResult, whose rows are JSON
 *     (@c json_t, from jansson) and whose errors are the closed #DbError
 *     enum.  Swapping the driver for one that speaks to something else
 *     changes nothing on this side.
 *
 *   - @b Anybody @b can @b call @b it.  Modules are opened with
 *     @c RTLD_LOCAL, so one module cannot resolve another's symbols; a
 *     database API that lived in the driver module would have no callers.
 *     These functions are in the ircd, which exports its symbols, so every
 *     module reaches them by linking against nothing at all -- the same way
 *     it reaches send_reply() or module_add_hook().
 *
 * @section db_async Everything is asynchronous
 *
 * A query is a network round trip and the core is single-threaded, so
 * db_query() never waits: it hands the query to the driver, which runs it in
 * a worker thread (see worker.h), and returns at once.  The answer arrives
 * later, in the main thread, as a call to the #DbResultFn the caller gave.
 *
 * "Later" means a different event loop iteration, and up to the configured
 * timeout -- five seconds at the very most, see #DB_TIMEOUT_MAX_MS.  The
 * client that asked for the work may well have quit by then, so a callback
 * must never rely on a @c struct @c Client* captured at submit time.  Store
 * the numnick instead, exactly as a worker task does, and look it up again
 * (see worker_task_set_client()).
 *
 * @section db_life Who owns what
 *
 * The #DbQuery, its #DbParam list and every string they point at belong to
 * the caller and are copied before db_query() returns; they may be stack
 * objects.  The #DbResult handed to the callback belongs to the driver and
 * is released as soon as the callback returns -- a consumer that wants to
 * keep the rows calls @c json_incref() on #DbResult::data.
 *
 * A call is tracked against the module that made it.  Unloading that module
 * drops its pending callbacks rather than calling into code that is no
 * longer mapped, so a module does not have to drain the database before it
 * can be unloaded.
 *
 * See doc/readme.database.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

struct Client;
struct ModuleHandle;

/** jansson's @c json_t.
 *
 * Declared, never defined here: the ircd does not link against jansson and
 * never looks inside one of these.  A consumer that means to read
 * #DbResult::data includes @c \<jansson.h\>, which defines the same struct,
 * and @c json_t and @c struct @c json_t are then the same type.
 */
struct json_t;

/** Longest translated error message, without the NUL. */
#define DB_ERRMSG_LEN 255

/** Hard ceiling on any database timeout, in milliseconds.
 *
 * Five seconds, and the configuration cannot raise it.  A query that has
 * not answered in five seconds has failed as far as an IRC server is
 * concerned; waiting longer only holds a pooled connection hostage.
 */
#define DB_TIMEOUT_MAX_MS 5000

/** Timeout used when @c Database{} does not set one, in milliseconds. */
#define DB_TIMEOUT_DEFAULT_MS 5000

/** Most parameters one query may carry. */
#define DB_MAX_PARAMS 64

/** Largest pool #DatabaseConf accepts for one role. */
#define DB_MAX_POOL 64

/** Pool size used when @c Database{} does not set one. */
#define DB_DEFAULT_POOL 4

/** Which side of the configuration a query is routed to.
 *
 * A deployment with a read replica points @c read at it and @c write at the
 * primary; one without simply sets @c dsn, and both roles resolve to it.
 */
enum DbRole {
  DB_ROLE_READ,    /**< Read-only work: db_query(). */
  DB_ROLE_WRITE,   /**< Anything that changes data: db_exec(). */
  DB_ROLE_LAST     /**< Number of roles. */
};

/** Type of a bound parameter.
 *
 * These are abstract types, not a database's own: the driver maps each one
 * to whatever its server calls that, and a query that binds #DB_TYPE_TEXT
 * gets a text parameter wherever it runs.  #DB_TYPE_UNKNOWN leaves the type
 * to the server to infer from the statement, which is usually what a
 * hand-written @c WHERE clause wants.
 */
enum DbType {
  DB_TYPE_UNKNOWN = 0, /**< Let the server infer it from the statement. */
  DB_TYPE_NULL,        /**< SQL NULL; #DbParam::value is ignored. */
  DB_TYPE_BOOL,        /**< Boolean. */
  DB_TYPE_SMALLINT,    /**< 16-bit signed integer. */
  DB_TYPE_INT,         /**< 32-bit signed integer. */
  DB_TYPE_BIGINT,      /**< 64-bit signed integer. */
  DB_TYPE_FLOAT,       /**< Double-precision float. */
  DB_TYPE_NUMERIC,     /**< Exact decimal, carried as text. */
  DB_TYPE_TEXT,        /**< Character string. */
  DB_TYPE_BYTEA,       /**< Byte string. */
  DB_TYPE_JSON,        /**< JSON document, carried as text. */
  DB_TYPE_UUID,        /**< UUID. */
  DB_TYPE_DATE,        /**< Calendar date. */
  DB_TYPE_TIME,        /**< Time of day. */
  DB_TYPE_TIMESTAMP,   /**< Timestamp without time zone. */
  DB_TYPE_TIMESTAMPTZ, /**< Timestamp with time zone. */
  DB_TYPE_INET,        /**< IP address or network. */
  DB_TYPE_LAST         /**< Number of types. */
};

/** How a parameter's bytes are encoded.
 *
 * #DB_FORMAT_TEXT is the one to use.  #DB_FORMAT_BINARY exists because the
 * wire protocol has it, and it comes with a restriction that follows from
 * #DbParam carrying no length: the driver can only tell how long a binary
 * value is when the type is fixed-width (#DB_TYPE_BOOL, the integers,
 * #DB_TYPE_FLOAT, #DB_TYPE_UUID and the date/time types).  A
 * variable-length type in binary format is refused with #DB_ERR_PARAM; send
 * it as text, which for #DB_TYPE_BYTEA means the @c \\x hex form.
 */
enum DbFormat {
  DB_FORMAT_TEXT = 0,   /**< #DbParam::value is a NUL-terminated string. */
  DB_FORMAT_BINARY = 1  /**< #DbParam::value is the type's wire encoding. */
};

/** What went wrong, in terms that do not name a database product.
 *
 * The driver translates its own status codes into these.  A consumer
 * switches on the code and shows #DbErrDetails::message; it never sees an
 * SQLSTATE, an errno or a libpq string.
 */
enum DbError {
  DB_OK = 0,            /**< No error. */
  DB_ERR_UNAVAILABLE,   /**< No driver loaded, or its pool is not running. */
  DB_ERR_CONFIG,        /**< No @c Database{} block, or it is unusable. */
  DB_ERR_CONNECT,       /**< Could not reach the database. */
  DB_ERR_TIMEOUT,       /**< Took longer than the configured timeout. */
  DB_ERR_BUSY,          /**< Pool queue is full; retry later. */
  DB_ERR_PARAM,         /**< The query or its parameters are malformed. */
  DB_ERR_SYNTAX,        /**< The database rejected the statement. */
  DB_ERR_UNDEFINED,     /**< No such table, column or function. */
  DB_ERR_PERMISSION,    /**< The database refused on privilege grounds. */
  DB_ERR_UNIQUE,        /**< Unique constraint violated. */
  DB_ERR_FOREIGN_KEY,   /**< Foreign key constraint violated. */
  DB_ERR_NOT_NULL,      /**< NOT NULL constraint violated. */
  DB_ERR_CONSTRAINT,    /**< Some other constraint violated. */
  DB_ERR_DATA,          /**< Bad value: wrong type, out of range, no zero. */
  DB_ERR_READONLY,      /**< A write was sent to a read-only connection. */
  DB_ERR_RETRY,         /**< Deadlock or serialization failure; retryable. */
  DB_ERR_RESOURCE,      /**< Out of memory, connections or disk. */
  DB_ERR_INTERNAL,      /**< Anything the driver could not classify. */
  DB_ERR_LAST           /**< Number of error codes. */
};

/** An error, translated.
 *
 * #dberr_code is #DB_OK exactly when the query succeeded, which is the one
 * test a caller needs; #dberr_message is for the log or for the operator and
 * is always NUL-terminated, empty when there is no error.
 */
struct DbErrDetails {
  enum DbError dberr_code;                 /**< What kind of failure. */
  char         dberr_message[DB_ERRMSG_LEN + 1]; /**< Human-readable text. */
};

/** One bound parameter.
 *
 * Parameters are bound, never pasted: the statement travels with @c $1,
 * @c $2 placeholders and these values travel beside it, so a value can
 * never be read as SQL however it is spelled.
 */
struct DbParam {
  enum DbType   type;    /**< Type to bind it as. */
  const char*   value;   /**< The value; NULL means SQL NULL. */
  enum DbFormat format;  /**< How #value is encoded. */
};

/** A statement and the values to bind to it.
 *
 * @code
 *   struct DbParam  nick  = { DB_TYPE_TEXT, cli_name(sptr), DB_FORMAT_TEXT };
 *   struct DbParam* args[] = { &nick, NULL };
 *   struct DbQuery  q = { "SELECT * FROM accounts WHERE nick = $1", args };
 *
 *   db_query(mod, &q, account_loaded, 0);
 * @endcode
 */
struct DbQuery {
  const char*      sql;     /**< The statement, with @c $n placeholders. */
  struct DbParam** params;  /**< NULL-terminated; NULL for none at all. */
};

/** The answer to one query.
 *
 * On success #data is a JSON array with one object per row, mapping column
 * name to value -- empty for a statement that returns nothing, which is not
 * an error -- and #err.dberr_code is #DB_OK.  On failure #data is NULL and
 * #err says what happened.
 *
 * The result belongs to the driver and is released when the callback
 * returns; keep the rows with @c json_incref() if they are needed after
 * that.
 */
struct DbResult {
  struct json_t*      data;  /**< Array of row objects, or NULL on error. */
  struct DbErrDetails err;   /**< #DB_OK, or what went wrong. */
  unsigned int        rows;  /**< Rows returned, or rows a write affected. */
};

/** Receives the answer to a query.  Runs in the main thread.
 *
 * Called exactly once for every call db_query() or db_exec() accepted,
 * unless the module that made the call is unloaded first.
 * @param[in] res The result.  Valid only until this returns.
 * @param[in] user The pointer handed to db_query().
 */
typedef void (*DbResultFn)(const struct DbResult* res, void* user);

/*
 * Asking for work.  This is the whole consumer API.
 */

/** Run a read-only query.
 *
 * Routed to the @c read side of the @c Database{} block when it has one.
 *
 * @param[in] mod Handle passed to the module's mi_init, or NULL from the
 *   core.  The callback is dropped rather than called if this module is
 *   unloaded before the answer arrives.
 * @param[in] query Statement and parameters.  Copied before this returns.
 * @param[in] cb Called in the main thread with the result.  May be NULL for
 *   a query whose answer nobody needs.
 * @param[in] user Passed through to \a cb.
 * @return #DB_OK when the query was accepted and \a cb will run later.
 *   Anything else is a refusal here and now, and \a cb will never run.
 */
extern enum DbError db_query(struct ModuleHandle* mod,
                             const struct DbQuery* query,
                             DbResultFn cb, void* user);

/** Run a statement that changes data.
 *
 * The same as db_query() except that it is routed to the @c write side.
 * @param[in] mod Handle passed to the module's mi_init, or NULL.
 * @param[in] query Statement and parameters.  Copied before this returns.
 * @param[in] cb Called in the main thread with the result, or NULL.
 * @param[in] user Passed through to \a cb.
 * @return #DB_OK when accepted; any other code is an immediate refusal.
 */
extern enum DbError db_exec(struct ModuleHandle* mod,
                            const struct DbQuery* query,
                            DbResultFn cb, void* user);

/** Non-zero when a driver is loaded and the configuration is usable.
 *
 * A module that needs a database checks this in its mi_init so it can say
 * so once, rather than failing every query in silence.
 */
extern int db_available(void);

/** Name of the loaded driver ("postgres"), or NULL if there is none. */
extern const char* db_driver_name(void);

/** A short description of \a code, for a message to an operator.
 * @param[in] code Error code.
 * @return A constant string; never NULL.
 */
extern const char* db_strerror(enum DbError code);

/*
 * The Database{} block.
 *
 * The configuration is read and kept whether or not a driver is loaded --
 * the block is not an error on a server with no database module, it simply
 * sits here until something asks for it.
 */

/** What @c Database{} said.
 *
 * Strings are owned by the configuration and stay valid until the next
 * rehash; a driver that keeps them past that copies them, and watches
 * db_conf_generation() to notice the change.
 */
struct DatabaseConf {
  char* dbconf_dsn;                  /**< Connection string.  Required. */
  char* dbconf_role_dsn[DB_ROLE_LAST]; /**< Per-role override, or NULL. */
  int   dbconf_role_pool[DB_ROLE_LAST]; /**< Connections per role. */
  int   dbconf_timeout_ms;           /**< Query timeout, at most
                                          #DB_TIMEOUT_MAX_MS. */
  unsigned int dbconf_generation;    /**< Bumped every time this changes. */
};

/** The configuration, or NULL if ircd.conf has no @c Database{} block. */
extern const struct DatabaseConf* db_conf(void);

/** Connection string for \a role: its own if it has one, else the common one.
 * @param[in] role Role to resolve.
 * @return The DSN, or NULL when there is no configuration.
 */
extern const char* db_conf_dsn(enum DbRole role);

/** Connections the configuration wants for \a role. */
extern int db_conf_pool(enum DbRole role);

/** Query timeout in milliseconds, already clamped to #DB_TIMEOUT_MAX_MS. */
extern int db_conf_timeout(void);

/** Counter that changes whenever the configuration does.
 *
 * A driver records this when it builds its pools and compares on rehash;
 * an unchanged generation means nothing it cares about moved.
 */
extern unsigned int db_conf_generation(void);

/*
 * Instrumentation.
 */

/** Queries accepted and not yet answered. */
extern unsigned int db_calls_pending(void);
/** Queries accepted since the server started. */
extern unsigned int db_calls_total(void);
/** Queries that came back with an error. */
extern unsigned int db_calls_failed(void);

/*
 * The driver side.  A database module implements these; nothing else calls
 * them.
 */

/** What the server asks of a driver.
 *
 * One statically allocated object per driver module, registered from its
 * mi_init.  Every entry runs in the main thread.
 */
struct DbDriver {
  /** Short name, for logs and db_driver_name(). */
  const char* dbdrv_name;

  /** Start one query.
   *
   * The driver copies whatever it needs out of \a query -- which is the
   * caller's and does not outlive this call -- queues the work, and
   * reports the answer later with db_complete(\a id, ...).
   *
   * @param[in] id Handle to hand back to db_complete().
   * @param[in] query Statement and parameters.
   * @param[in] role Which side to run it on.
   * @return #DB_OK when the query was queued; any other code refuses it,
   *   and db_complete() must then not be called for \a id.
   */
  enum DbError (*dbdrv_submit)(unsigned long id,
                               const struct DbQuery* query,
                               enum DbRole role);

  /** Release a @c json_t the driver produced.
   *
   * The ircd does not link against jansson, so it cannot decrement a
   * reference count itself; it calls this instead, once per result,
   * including for a result whose callback was dropped.
   * @param[in] data Value from db_complete(), possibly NULL.
   */
  void (*dbdrv_release)(struct json_t* data);
};

/** Register the loaded module as the database driver.
 *
 * There is one driver at a time: a second registration is refused rather
 * than replacing the first, so two database modules in ircd.conf produce an
 * error instead of a coin flip.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] driver Static description of the driver.
 * @return Non-zero on success.
 */
extern int db_register_driver(struct ModuleHandle* mod,
                              const struct DbDriver* driver);

/** Withdraw a driver.
 *
 * Every query still in flight is failed with #DB_ERR_UNAVAILABLE before
 * this returns, so no callback is left waiting for a module that has gone.
 * @param[in] mod Handle that registered it.
 */
extern void db_unregister_driver(struct ModuleHandle* mod);

/** Deliver the answer to query \a id.  Main thread only.
 *
 * Consumes the call: \a id is invalid afterwards, and the driver must call
 * this exactly once for every submit it accepted.
 *
 * @param[in] id Handle from #DbDriver::dbdrv_submit.
 * @param[in] data Rows as a JSON array, or NULL on failure.  Released
 *   through #DbDriver::dbdrv_release whether or not the callback runs.
 * @param[in] rows Rows returned, or rows a write affected.
 * @param[in] code #DB_OK, or what went wrong.
 * @param[in] message Translated message, or NULL to use db_strerror().
 */
extern void db_complete(unsigned long id, struct json_t* data,
                        unsigned int rows, enum DbError code,
                        const char* message);

/*
 * Server-side interface.  Not for use by modules.
 */

/** Reset the configuration; the parser calls this for each @c Database{}. */
extern void db_conf_clear(void);
/** Set the common or a per-role DSN.  \a role is -1 for the common one. */
extern void db_conf_set_dsn(int role, char* dsn);
/** Set the common or a per-role pool size.  \a role is -1 for all roles. */
extern void db_conf_set_pool(int role, int size);
/** Set the query timeout, in milliseconds; clamped on commit. */
extern void db_conf_set_timeout(int ms);
/** Finish a @c Database{} block: validate it and publish it.
 * @param[out] errstr Receives a reason when this returns zero.
 * @return Non-zero when the block was accepted.
 */
extern int db_conf_commit(const char** errstr);

/** Forget that a @c Database{} block was seen; before reading the file. */
extern void db_conf_unmark(void);
/** Drop the configuration if this pass had no @c Database{} block. */
extern void db_conf_sweep(void);

/** Drop a module's driver and its pending calls; module.c only. */
extern void db_drop_module(struct ModuleHandle* mod);
/** Release everything; main() only, at exit. */
extern void db_shutdown(void);

#endif /* INCLUDED_db_h */
