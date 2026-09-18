#ifndef INCLUDED_postgres_h
#define INCLUDED_postgres_h
/*
 * IRC - Internet Relay Chat, modules/workers/postgres/postgres.h
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
 * @brief Private interfaces of the PostgreSQL driver.
 *
 * Nothing here is visible to a consumer of the database API: a module that
 * wants to run a query includes @c db.h and calls db_query(), and never
 * learns that the answer came from libpq.  This header is for the driver's
 * own files.
 *
 * @section pg_threads Which thread is which
 *
 * The driver straddles the one boundary the server has, so every function
 * below says which side it is on.
 *
 *   - @b Main @b thread: the module's lifecycle, the pools' bookkeeping, and
 *     handing answers back to the caller.  Anything here may touch core
 *     state.
 *   - @b Connection @b thread: one dedicated worker per pooled connection,
 *     started with module_spawn_worker().  It owns exactly one @c PGconn,
 *     and it obeys the rule in worker.h without exception -- no core state,
 *     no @c MyMalloc(), no @c log_write().  It allocates with
 *     worker_alloc(), logs with worker_log(), and hands its results to the
 *     main thread with worker_post().
 *
 * The two share only the pool: its queue, its counters and its stop flag,
 * all under #PgPool::pgp_lock.  Everything else that crosses the boundary
 * is copied.
 */

#include "db.h"
#include "worker.h"

#include <jansson.h>
#include <libpq-fe.h>
#include <pthread.h>
#include <time.h>

/** Queued queries allowed per connection before the pool says it is busy.
 *
 * The queue exists to absorb a burst, not to store a backlog: a query that
 * waits behind sixteen others has already missed its timeout.
 */
#define PG_QUEUE_PER_CONN 16

/** Prepared statements one connection caches before it is recycled.
 *
 * Not an LRU: when the cache fills, the connection is closed and remade,
 * which discards every statement prepared on it at once.  Emptying it any
 * other way would mean sending a DEALLOCATE -- the one statement that
 * cannot itself go through the prepared path.  See pg_conn.c.
 */
#define PG_STMT_CACHE 128

/** Buckets in a connection's prepared-statement table. */
#define PG_STMT_BUCKETS 64

/** How long a connection thread sleeps between checks for a stop request. */
#define PG_POLL_INTERVAL_MS 100

/** Grace period for a cancelled query to acknowledge, in milliseconds.
 *
 * After a timeout the connection has to be drained before it can be used
 * again.  If the server has not answered the cancel within this, the
 * connection is thrown away and remade instead.
 */
#define PG_CANCEL_GRACE_MS 250

/** How long to wait before retrying a connection that failed, in seconds. */
#define PG_RECONNECT_DELAY 2

/* ------------------------------------------------------------------------
 * pg_error.c -- translating what the database says into enum DbError.
 * ------------------------------------------------------------------------ */

/** Translate an SQLSTATE into a #DbError.
 *
 * @param[in] sqlstate Five-character SQLSTATE, or NULL.
 * @return The closest #DbError; #DB_ERR_INTERNAL when there is nothing to
 *   go on.
 */
extern enum DbError pg_error_from_sqlstate(const char* sqlstate);

/** The message a consumer is allowed to see for \a code.
 *
 * Deliberately the driver's own words and never libpq's: a caller may put
 * this in a notice to a user, and a database's error text carries table
 * names, column names and fragments of the statement.  The detail is not
 * lost -- pg_error_log() puts it in the server log, where the operator is.
 *
 * @param[in] code Error code.
 * @return A constant string; never NULL.
 */
extern const char* pg_error_message(enum DbError code);

/** Record the database's own account of a failure.  Connection thread.
 *
 * @param[in] where Short description of what was being attempted.
 * @param[in] detail Text from libpq, or NULL.
 */
extern void pg_error_log(const char* where, const char* detail);

/* ------------------------------------------------------------------------
 * pg_types.c -- enum DbType on one side, PostgreSQL OIDs on the other.
 * ------------------------------------------------------------------------ */

/** The OID a #DbType binds as.
 * @param[in] type Abstract type.
 * @return The OID, or 0 to let the server infer it.
 */
extern Oid pg_type_oid(enum DbType type);

/** Length of a binary value of \a type, in bytes.
 *
 * #DbParam carries no length, so a binary parameter is only accepted for a
 * type whose width is fixed.
 * @param[in] type Abstract type.
 * @return The width, or -1 when the type has no fixed one.
 */
extern int pg_type_binary_length(enum DbType type);

/* ------------------------------------------------------------------------
 * pg_json.c -- a PGresult becomes a json_t array of row objects.
 * ------------------------------------------------------------------------ */

/** Convert the rows of \a res to JSON.  Connection thread.
 *
 * @param[in] res A result in text format.
 * @return A new JSON array of objects, or NULL if there was no memory.
 *   The caller owns the reference.
 */
extern json_t* pg_json_rows(const PGresult* res);

/* ------------------------------------------------------------------------
 * pg_conn.c -- one pooled connection and the thread that owns it.
 * ------------------------------------------------------------------------ */

struct PgPool;
struct PgConn;
struct PgRequest;

/** A deadline, on the monotonic clock.  Connection thread. */
struct PgDeadline {
  struct timespec pgd_at;   /**< When the work must be finished. */
};

/** Milliseconds on the monotonic clock.  Worker thread.
 *
 * Only good for subtracting from itself, which is all anything here wants:
 * how long a migration's script took.
 */
extern long pg_monotonic_ms(void);

/** Arm a deadline \a ms milliseconds from now.
 * @param[out] deadline Deadline to arm.
 * @param[in] ms Milliseconds from now.
 */
extern void pg_deadline_set(struct PgDeadline* deadline, int ms);

/** Milliseconds left before \a deadline, or zero once it has passed.
 * @param[in] deadline Deadline to measure.
 */
extern int pg_deadline_left(const struct PgDeadline* deadline);

/** Wait for \a fd, for a stop request, or for the deadline.  Worker thread.
 *
 * The one place this driver ever waits.  Every deadline it honours -- a
 * connect, a result, a send buffer that will not drain -- goes through
 * here, which is what makes #DB_TIMEOUT_MAX_MS a promise rather than a hope.
 * @param[in] fd Descriptor to watch.
 * @param[in] forwrite Non-zero to wait for writability instead.
 * @param[in] stopfd worker_stop_fd(), or -1 for a thread with no worker of
 *   its own.
 * @param[in] deadline When to give up.
 * @return 1 when \a fd is ready, 0 when the deadline passed, -1 when the
 *   thread was asked to stop or the wait itself failed.
 */
extern int pg_socket_wait(int fd, int forwrite, int stopfd,
                          const struct PgDeadline* deadline);

/** Wait for the result of whatever was sent, honouring the deadline.
 *
 * Drains the connection completely -- libpq hands back results until it
 * returns NULL, and leaving one behind would desynchronise the next
 * statement on that connection.
 * @param[in] pg Connection to read.
 * @param[in] stopfd worker_stop_fd(), or -1.
 * @param[in] deadline When to give up.
 * @param[out] out Receives the first result, or NULL.  The caller clears it.
 * @return 0 when a result arrived, -1 on the deadline, -2 when the
 *   connection is no longer usable.
 */
extern int pg_collect(PGconn* pg, int stopfd,
                      const struct PgDeadline* deadline, PGresult** out);

/** Create a connection for \a pool.  Main thread.
 *
 * The socket is not opened here; the connection thread does that the first
 * time it has something to run.
 * @param[in] pool Pool the connection belongs to.
 * @param[in] index Position in the pool, for its name.
 * @return The connection, or NULL if there was no memory.
 */
extern struct PgConn* pg_conn_new(struct PgPool* pool, unsigned int index);

/** Release a connection.  Main thread, after its worker has stopped.
 * @param[in] conn Connection to release.
 */
extern void pg_conn_free(struct PgConn* conn);

/** Name the connection's worker thread carries.
 * @param[in] conn Connection to name.
 */
extern const char* pg_conn_name(const struct PgConn* conn);

/** The body of a connection thread.  Connection thread.
 *
 * Takes requests off the pool's queue until it is asked to stop, and posts
 * an answer for every one of them.
 * @param[in] worker This thread's worker.
 * @param[in] arg The #PgConn, as handed to module_spawn_worker().
 */
extern void pg_conn_main(struct Worker* worker, void* arg);

/* ------------------------------------------------------------------------
 * pg_migrate.c -- schema migrations, off the pool and in a transaction.
 * ------------------------------------------------------------------------ */

/** Start one migration.  Main thread.
 *
 * Copies everything it needs and hands the work to the worker pool.  See
 * pg_migrate.c for why a migration does not go through #PgPool at all.
 * @param[in] id Handle to complete the migration with.
 * @param[in] migration What to run.
 * @return #DB_OK when it was queued.
 */
extern enum DbError pg_migrate_submit(unsigned long id,
                                      const struct DbMigration* migration);

/* ------------------------------------------------------------------------
 * pg_json.c, again -- reading a result back, for a caller with no jansson.
 * ------------------------------------------------------------------------ */

/** Rows in \a data.  Main thread.
 * @param[in] data A JSON array of row objects, or NULL.
 */
extern unsigned int pg_json_count(json_t* data);

/** One column of one row, rendered as text.  Main thread.
 * @param[in] data A JSON array of row objects, or NULL.
 * @param[in] row Row index, from zero.
 * @param[in] column Column name.
 * @return The value, or NULL when there is no such row or column.  Points
 *   into a buffer that the next call overwrites.
 */
extern const char* pg_json_str(json_t* data, unsigned int row,
                               const char* column);

/** One column of one row, as an integer.  Main thread.
 * @param[in] data A JSON array of row objects, or NULL.
 * @param[in] row Row index, from zero.
 * @param[in] column Column name.
 * @return The value, or zero if it is not a number.
 */
extern long long pg_json_int(json_t* data, unsigned int row,
                             const char* column);

/* ------------------------------------------------------------------------
 * pg_pool.c -- the queue, the connections, and the counters.
 * ------------------------------------------------------------------------ */

/** One parameter, copied out of the caller's #DbParam.  Worker memory. */
struct PgBound {
  Oid   pgb_type;     /**< OID to bind as, or 0 to let the server infer. */
  char* pgb_value;    /**< The bytes, or NULL for SQL NULL. */
  int   pgb_length;   /**< Length in bytes; only read in binary format. */
  int   pgb_format;   /**< 0 text, 1 binary. */
};

/** One query on its way to a connection.  Worker memory throughout.
 *
 * Everything the connection thread needs is in here, copied: it never
 * dereferences anything the main thread owns.
 */
struct PgRequest {
  struct PgRequest* pgr_next;      /**< Next in the pool's queue. */
  unsigned long     pgr_id;        /**< Handle for db_complete(). */
  int               pgr_timeout_ms;/**< Deadline for the whole round trip. */
  char*             pgr_sql;       /**< The statement. */
  unsigned int      pgr_nparams;   /**< Number of parameters. */
  struct PgBound*   pgr_params;    /**< #pgr_nparams parameters, or NULL. */
};

/** Release a request and everything in it.  Either thread.
 * @param[in] req Request to release, or NULL.
 */
extern void pg_request_free(struct PgRequest* req);

/** Take the next request off \a pool, waiting for one.  Connection thread.
 *
 * Waits in short steps so that a stop request is noticed promptly.
 * @param[in] pool Pool to take from.
 * @param[in] worker The calling thread's worker, polled for a stop.
 * @return A request the caller now owns, or NULL when the thread should
 *   look at worker_stopping() again.
 */
extern struct PgRequest* pg_pool_take(struct PgPool* pool,
                                      struct Worker* worker);

/** Hand an answer back to the main thread.  Connection thread.
 *
 * Posts a task whose wt_done calls db_complete().  A JSON value handed over
 * here belongs to the main thread afterwards.
 * @param[in] worker The calling thread's worker.
 * @param[in] id Handle from the request.
 * @param[in] data Rows, or NULL.  The reference is transferred.
 * @param[in] rows Rows returned or affected.
 * @param[in] code Result code.
 */
extern void pg_pool_answer(struct Worker* worker, unsigned long id,
                           json_t* data, unsigned int rows,
                           enum DbError code);

/** Read a pool's connection string.  Connection thread.
 *
 * A copy the pool owns for as long as its threads run, so the connection
 * thread may read it without a lock.
 * @param[in] pool Pool to ask.
 */
extern const char* pg_pool_dsn(const struct PgPool* pool);

/** Which role this pool serves, as a word for logs.
 * @param[in] pool Pool to ask.
 */
extern const char* pg_pool_label(const struct PgPool* pool);

/** The deadline this pool gives every query, in milliseconds.
 *
 * The pool's own copy of the configured timeout, taken when it was built:
 * a connection thread must not read the configuration, which belongs to the
 * main thread.
 * @param[in] pool Pool to ask.
 */
extern int pg_pool_timeout(const struct PgPool* pool);

/** Count one outcome against a pool.  Connection thread.
 * @param[in] pool Pool to count against.
 * @param[in] code How the query ended.
 */
extern void pg_pool_count(struct PgPool* pool, enum DbError code);

/** Count a connection attempt.  Connection thread.
 * @param[in] pool Pool to count against.
 * @param[in] ok Non-zero if the connection succeeded.
 */
extern void pg_pool_count_connect(struct PgPool* pool, int ok);

/*
 * The pools themselves.  Main thread only.
 */

/** Bring the pools in line with the configuration.  Main thread.
 *
 * Cheap and idempotent: it does nothing at all unless db_conf_generation()
 * has moved since the pools were built, in which case they are torn down so
 * the next query builds them from the new configuration.
 */
extern void pg_pools_sync(void);

/** The pool for \a role, started if it was not running.  Main thread.
 *
 * @param[in] role Role to serve.
 * @param[out] err Receives the reason when this returns NULL.
 * @return A running pool, or NULL.
 */
extern struct PgPool* pg_pools_get(enum DbRole role, enum DbError* err);

/** Queue \a query on \a pool.  Main thread.
 *
 * Copies the statement and its parameters into worker memory first; the
 * caller's #DbQuery does not have to outlive the call.
 * @param[in] pool Pool to queue on.
 * @param[in] id Handle for db_complete().
 * @param[in] query Statement and parameters.
 * @return #DB_OK when the query was queued.
 */
extern enum DbError pg_pools_submit(struct PgPool* pool, unsigned long id,
                                    const struct DbQuery* query);

/** Stop every pool and fail whatever was queued.  Main thread.
 *
 * Waits for the connection threads, which can take as long as one query
 * timeout.  Queries still in the queue are completed with
 * #DB_ERR_UNAVAILABLE rather than dropped.
 */
extern void pg_pools_stop(void);

/** Write a line per pool to the server log.  Main thread. */
extern void pg_pools_report(void);

/** The module's handle, for module_spawn_worker().  Main thread only. */
extern struct ModuleHandle* pg_module;

#endif /* INCLUDED_postgres_h */
