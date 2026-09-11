/*
 * IRC - Internet Relay Chat, modules/workers/postgres/pg_conn.c
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
 * @brief One connection, one thread, and a deadline it cannot exceed.
 *
 * Everything in this file after pg_conn_new() runs in a connection thread
 * and obeys worker.h to the letter: the only memory it allocates comes from
 * worker_alloc() or from jansson, the only logging it does is worker_log(),
 * and the only thing it shares with the main thread is the pool's queue.
 *
 * @section pg_prepared Prepared statements, always
 *
 * A statement is never assembled from a value.  It travels with @c $1,
 * @c $2 placeholders, it is prepared once per connection and cached, and the
 * values travel beside it in their own array.  That is what makes injection
 * a non-question rather than a review item: there is no code path in this
 * driver that concatenates a parameter into SQL, and no entry point that
 * would let a caller ask for one.
 *
 * @section pg_deadline The five second rule
 *
 * db.h promises that no query outlives #DB_TIMEOUT_MAX_MS, and a promise
 * that a stalled network can break is not one.  So the round trip is driven
 * through libpq's non-blocking entry points -- @c PQsendPrepare and
 * @c PQsendQueryPrepared, which are @c PQprepare and @c PQexecPrepared with
 * the waiting left to the caller -- and every wait is a poll() against the
 * deadline.  When the deadline passes, the query is cancelled, the
 * connection is drained, and the caller is told #DB_ERR_TIMEOUT.  A blocking
 * @c PQexecPrepared would have no such moment: a TCP connection to a machine
 * that has stopped answering takes minutes to fail, and the pooled
 * connection would be gone for all of them.
 *
 * The server is told the same deadline at connect time, as
 * @c statement_timeout, so a query that survives a lost cancel packet still
 * ends on its own.
 */
#include "postgres.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** A statement this connection has prepared. */
struct PgStmt {
  struct PgStmt* pgs_hnext;    /**< Next in its hash bucket. */
  unsigned int   pgs_hash;     /**< Hash of #pgs_sql. */
  unsigned int   pgs_nparams;  /**< Parameters it was prepared with. */
  Oid*           pgs_types;    /**< Their types, or NULL for none. */
  char*          pgs_sql;      /**< The statement text. */
  char           pgs_name[32]; /**< Name it is prepared under. */
};

/** One pooled connection. */
struct PgConn {
  struct PgPool* pgc_pool;     /**< Pool this belongs to. */
  PGconn*        pgc_pg;       /**< libpq connection, or NULL. */
  time_t         pgc_retry_at; /**< Monotonic second to retry connecting. */
  unsigned long  pgc_seq;      /**< Counter behind #PgStmt::pgs_name. */
  unsigned int   pgc_nstmts;   /**< Statements in the cache. */
  struct PgStmt* pgc_buckets[PG_STMT_BUCKETS]; /**< The cache itself. */
  char           pgc_name[WORKER_NAMELEN + 1]; /**< Name of its thread. */
};

/* ------------------------------------------------------------------------
 * Deadlines.
 * ------------------------------------------------------------------------ */

/** Now, on the monotonic clock.
 * @param[out] now Receives the time.
 */
static void pg_now(struct timespec* now)
{
  if (clock_gettime(CLOCK_MONOTONIC, now)) {
    now->tv_sec = 0;
    now->tv_nsec = 0;
  }
}

void pg_deadline_set(struct PgDeadline* deadline, int ms)
{
  if (ms < 0)
    ms = 0;
  if (ms > DB_TIMEOUT_MAX_MS)
    ms = DB_TIMEOUT_MAX_MS;     /* the ceiling, one last time */

  pg_now(&deadline->pgd_at);

  deadline->pgd_at.tv_sec += ms / 1000;
  deadline->pgd_at.tv_nsec += (long) (ms % 1000) * 1000000L;
  if (deadline->pgd_at.tv_nsec >= 1000000000L) {
    deadline->pgd_at.tv_sec++;
    deadline->pgd_at.tv_nsec -= 1000000000L;
  }
}

int pg_deadline_left(const struct PgDeadline* deadline)
{
  struct timespec now;
  long ms;

  pg_now(&now);

  ms = (long) (deadline->pgd_at.tv_sec - now.tv_sec) * 1000L
     + (deadline->pgd_at.tv_nsec - now.tv_nsec) / 1000000L;

  if (ms < 0)
    return 0;
  if (ms > DB_TIMEOUT_MAX_MS)
    return DB_TIMEOUT_MAX_MS;

  return (int) ms;
}

/** Wait for \a fd, for a stop request, or for the deadline.
 *
 * @param[in] fd Descriptor to watch.
 * @param[in] forwrite Non-zero to wait for writability instead.
 * @param[in] stopfd worker_stop_fd(), or -1.
 * @param[in] deadline When to give up.
 * @return 1 when \a fd is ready, 0 when the deadline passed, -1 when the
 *   thread was asked to stop or the wait itself failed.
 */
static int pg_wait(int fd, int forwrite, int stopfd,
                   const struct PgDeadline* deadline)
{
  struct pollfd fds[2];
  nfds_t nfds = 1;
  int left;
  int n;

  if (fd < 0)
    return -1;

  fds[0].fd = fd;
  fds[0].events = (short) (forwrite ? POLLOUT : POLLIN);
  fds[0].revents = 0;

  if (stopfd >= 0) {
    fds[1].fd = stopfd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    nfds = 2;
  }

  for (;;) {
    if ((left = pg_deadline_left(deadline)) <= 0)
      return 0;

    n = poll(fds, nfds, left);

    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return 0;

    /* A stop outranks a ready socket: the server is waiting for this
     * thread to return, and the query is going to be failed anyway.
     */
    if (nfds == 2 && fds[1].revents)
      return -1;
    if (fds[0].revents)
      return 1;
  }
}

/* ------------------------------------------------------------------------
 * The prepared statement cache.
 * ------------------------------------------------------------------------ */

/** Hash \a sql, for the cache's buckets.
 * @param[in] sql Statement text.
 * @return Its hash.
 */
static unsigned int pg_stmt_hash(const char* sql)
{
  unsigned int hash = 2166136261u;   /* FNV-1a */

  while (*sql) {
    hash ^= (unsigned char) *sql++;
    hash *= 16777619u;
  }

  return hash;
}

/** Release one cached statement.
 * @param[in] stmt Statement to release.
 */
static void pg_stmt_free(struct PgStmt* stmt)
{
  worker_free(stmt->pgs_sql);
  worker_free(stmt->pgs_types);
  worker_free(stmt);
}

/** Empty the cache, without telling the server.
 *
 * Only correct when the connection is going away with it -- which is the
 * only time it is called: closing the connection discards every statement
 * prepared on it, so there is nothing left to deallocate.
 * @param[in,out] conn Connection whose cache to empty.
 */
static void pg_stmt_clear(struct PgConn* conn)
{
  struct PgStmt* stmt;
  struct PgStmt* next;
  unsigned int bucket;

  for (bucket = 0; bucket < PG_STMT_BUCKETS; bucket++) {
    for (stmt = conn->pgc_buckets[bucket]; stmt; stmt = next) {
      next = stmt->pgs_hnext;
      pg_stmt_free(stmt);
    }
    conn->pgc_buckets[bucket] = 0;
  }

  conn->pgc_nstmts = 0;
}

/** Find the statement prepared for \a req, if there is one.
 *
 * The types are part of the key, not just the text: a statement prepared
 * with its parameters declared as text is a different statement from the
 * same text prepared with them declared as integers, and reusing one for
 * the other would bind the wrong types.
 *
 * @param[in,out] conn Connection to search.
 * @param[in] req Request to match.
 * @param[in] hash Hash of the request's SQL.
 * @return The cached statement, or NULL.
 */
static struct PgStmt* pg_stmt_find(struct PgConn* conn,
                                   const struct PgRequest* req,
                                   unsigned int hash)
{
  struct PgStmt* stmt;
  unsigned int n;

  for (stmt = conn->pgc_buckets[hash % PG_STMT_BUCKETS]; stmt;
       stmt = stmt->pgs_hnext) {
    if (stmt->pgs_hash != hash || stmt->pgs_nparams != req->pgr_nparams)
      continue;
    if (strcmp(stmt->pgs_sql, req->pgr_sql))
      continue;

    for (n = 0; n < req->pgr_nparams; n++)
      if (stmt->pgs_types[n] != req->pgr_params[n].pgb_type)
        break;

    if (n == req->pgr_nparams)
      return stmt;
  }

  return 0;
}

/* ------------------------------------------------------------------------
 * Talking to the server.
 * ------------------------------------------------------------------------ */

/** Wait for the result of whatever was sent, honouring the deadline.
 *
 * Drains the connection completely -- libpq hands back results until it
 * returns NULL, and leaving one behind would desynchronise the next query.
 *
 * @param[in] conn Connection to read.
 * @param[in] stopfd worker_stop_fd(), or -1.
 * @param[in] deadline When to give up.
 * @param[out] out Receives the first result, or NULL.  The caller clears it.
 * @return 0 when a result arrived, -1 on the deadline, -2 when the
 *   connection is no longer usable.
 */
static int pg_collect(struct PgConn* conn, int stopfd,
                      const struct PgDeadline* deadline, PGresult** out)
{
  PGresult* first = 0;
  PGresult* res;
  int flushed;

  *out = 0;

  /* Push the request out first; a full send buffer is a wait like any
   * other, and it is on the same deadline.
   */
  while ((flushed = PQflush(conn->pgc_pg)) > 0) {
    int ready = pg_wait(PQsocket(conn->pgc_pg), 1, stopfd, deadline);

    if (ready <= 0)
      return ready == 0 ? -1 : -2;
  }
  if (flushed < 0)
    return -2;

  for (;;) {
    while (PQisBusy(conn->pgc_pg)) {
      int ready = pg_wait(PQsocket(conn->pgc_pg), 0, stopfd, deadline);

      if (ready <= 0) {
        /* Whatever has arrived so far is of no use to anybody now, and the
         * caller is about to cancel or close: release it here so that no
         * error path has to remember to.
         */
        PQclear(first);
        return ready == 0 ? -1 : -2;
      }

      if (!PQconsumeInput(conn->pgc_pg)) {
        pg_error_log("reading a result", PQerrorMessage(conn->pgc_pg));
        PQclear(first);
        return -2;
      }
    }

    if (!(res = PQgetResult(conn->pgc_pg)))
      break;

    /* The first result is the answer; anything after it can only come from
     * a statement this driver did not send, and is discarded.
     */
    if (first)
      PQclear(res);
    else
      first = res;
  }

  *out = first;

  return 0;
}

/** Cancel whatever is running and get the connection back to idle.
 *
 * @param[in,out] conn Connection to rescue.
 * @param[in] stopfd worker_stop_fd(), or -1.
 * @return Non-zero when the connection is usable again.
 */
static int pg_cancel(struct PgConn* conn, int stopfd)
{
  struct PgDeadline grace;
  PGcancel* cancel;
  PGresult* res;
  char errbuf[256];

  if ((cancel = PQgetCancel(conn->pgc_pg))) {
    if (!PQcancel(cancel, errbuf, sizeof(errbuf)))
      pg_error_log("cancelling a query", errbuf);
    PQfreeCancel(cancel);
  }

  /* The cancel is a request, not a guarantee: the server still owes a
   * result, and until it arrives the connection cannot be reused.  Wait a
   * moment for it, then give up on the connection rather than on the pool.
   */
  pg_deadline_set(&grace, PG_CANCEL_GRACE_MS);

  if (pg_collect(conn, stopfd, &grace, &res))
    return 0;

  PQclear(res);

  return PQstatus(conn->pgc_pg) == CONNECTION_OK;
}

/** Close the connection and forget everything prepared on it.
 * @param[in,out] conn Connection to close.
 */
static void pg_disconnect(struct PgConn* conn)
{
  if (conn->pgc_pg) {
    PQfinish(conn->pgc_pg);
    conn->pgc_pg = 0;
  }

  pg_stmt_clear(conn);
}

/** Prepare and run one statement, with no parameters of its own to bind.
 *
 * Used for the session setup, which has to go through the same prepared
 * path as everything else.
 * @param[in,out] conn Connection to use.
 * @param[in] stopfd worker_stop_fd(), or -1.
 * @param[in] deadline When to give up.
 * @param[in] name Name to prepare it under.
 * @param[in] sql The statement.
 * @param[in] value Its one parameter, as text.
 * @return Non-zero on success.
 */
static int pg_run_setup(struct PgConn* conn, int stopfd,
                        const struct PgDeadline* deadline,
                        const char* name, const char* sql, const char* value)
{
  PGresult* res;
  int ok;

  if (!PQsendPrepare(conn->pgc_pg, name, sql, 1, 0))
    return 0;
  if (pg_collect(conn, stopfd, deadline, &res) || !res)
    return 0;

  ok = PQresultStatus(res) == PGRES_COMMAND_OK;
  PQclear(res);
  if (!ok)
    return 0;

  if (!PQsendQueryPrepared(conn->pgc_pg, name, 1, &value, 0, 0, 0))
    return 0;
  if (pg_collect(conn, stopfd, deadline, &res) || !res)
    return 0;

  ok = PQresultStatus(res) == PGRES_TUPLES_OK;
  PQclear(res);

  return ok;
}

/** Open the connection, if it is not open already.
 *
 * @param[in,out] conn Connection to bring up.
 * @param[in] stopfd worker_stop_fd(), or -1.
 * @param[in] deadline When to give up.
 * @return #DB_OK when there is a usable connection.
 */
static enum DbError pg_connect(struct PgConn* conn, int stopfd,
                               const struct PgDeadline* deadline)
{
  PostgresPollingStatusType polling = PGRES_POLLING_WRITING;
  struct timespec now;
  char timeout[32];
  PGconn* pg;

  if (conn->pgc_pg && PQstatus(conn->pgc_pg) == CONNECTION_OK)
    return DB_OK;

  pg_disconnect(conn);

  /* A database that is down would otherwise be dialled once per queued
   * query, each attempt waiting out the whole deadline.  One attempt every
   * few seconds is enough to notice it coming back.
   */
  pg_now(&now);
  if (conn->pgc_retry_at && now.tv_sec < conn->pgc_retry_at)
    return DB_ERR_CONNECT;

  if (!(pg = PQconnectStart(pg_pool_dsn(conn->pgc_pool))))
    return DB_ERR_RESOURCE;

  if (PQstatus(pg) == CONNECTION_BAD) {
    pg_error_log("connecting", PQerrorMessage(pg));
    PQfinish(pg);
    conn->pgc_retry_at = now.tv_sec + PG_RECONNECT_DELAY;
    pg_pool_count_connect(conn->pgc_pool, 0);
    return DB_ERR_CONNECT;
  }

  for (;;) {
    int ready = pg_wait(PQsocket(pg), polling == PGRES_POLLING_WRITING,
                        stopfd, deadline);

    if (ready <= 0) {
      PQfinish(pg);
      conn->pgc_retry_at = now.tv_sec + PG_RECONNECT_DELAY;
      pg_pool_count_connect(conn->pgc_pool, 0);
      return ready == 0 ? DB_ERR_TIMEOUT : DB_ERR_UNAVAILABLE;
    }

    polling = PQconnectPoll(pg);

    if (polling == PGRES_POLLING_OK)
      break;

    if (polling == PGRES_POLLING_FAILED) {
      pg_error_log("connecting", PQerrorMessage(pg));
      PQfinish(pg);
      conn->pgc_retry_at = now.tv_sec + PG_RECONNECT_DELAY;
      pg_pool_count_connect(conn->pgc_pool, 0);
      return DB_ERR_CONNECT;
    }
  }

  if (PQsetnonblocking(pg, 1)) {
    pg_error_log("connecting", PQerrorMessage(pg));
    PQfinish(pg);
    conn->pgc_retry_at = now.tv_sec + PG_RECONNECT_DELAY;
    pg_pool_count_connect(conn->pgc_pool, 0);
    return DB_ERR_RESOURCE;
  }

  conn->pgc_pg = pg;
  conn->pgc_retry_at = 0;
  pg_pool_count_connect(conn->pgc_pool, 1);

  /* Arm the server side of the deadline.  set_config() rather than SET
   * because it is a SELECT, so it goes through prepare-and-execute like
   * every other statement here; this driver has no path that sends a
   * statement any other way.  Failing is survivable -- the client-side
   * deadline still holds -- so it is logged and carried on from.
   */
  sprintf(timeout, "%d", pg_pool_timeout(conn->pgc_pool));
  if (!pg_run_setup(conn, stopfd, deadline, "ircu_pg_setup",
                    "select set_config('statement_timeout', $1, false)",
                    timeout))
    pg_error_log("setting statement_timeout", PQerrorMessage(pg));

  worker_log("postgres: %s connected (%s pool)", conn->pgc_name,
             pg_pool_label(conn->pgc_pool));

  return DB_OK;
}

/** Prepare \a req's statement, or find it already prepared.
 *
 * @param[in,out] conn Connection to prepare on.
 * @param[in] req Request whose statement is wanted.
 * @param[in] stopfd worker_stop_fd(), or -1.
 * @param[in] deadline When to give up.
 * @param[out] stmt_out Receives the statement.
 * @return #DB_OK on success.
 */
static enum DbError pg_prepare(struct PgConn* conn, struct PgRequest* req,
                               int stopfd, const struct PgDeadline* deadline,
                               struct PgStmt** stmt_out)
{
  unsigned int hash = pg_stmt_hash(req->pgr_sql);
  struct PgStmt* stmt;
  Oid types[DB_MAX_PARAMS];
  PGresult* res;
  enum DbError code;
  unsigned int n;
  int collected;

  if ((*stmt_out = pg_stmt_find(conn, req, hash)))
    return DB_OK;

  /* The cache is full.  Rather than send a DEALLOCATE -- the one statement
   * this driver would have to send outside the prepared path -- the
   * connection is closed, which discards every statement prepared on it at
   * once.  With a cache this size that is a thing that happens to a module
   * generating statement text per call, and is a hint that it should not.
   */
  if (conn->pgc_nstmts >= PG_STMT_CACHE) {
    worker_log("postgres: %s recycling: %u prepared statements cached",
               conn->pgc_name, conn->pgc_nstmts);
    pg_disconnect(conn);
    if ((code = pg_connect(conn, stopfd, deadline)) != DB_OK)
      return code;
  }

  for (n = 0; n < req->pgr_nparams; n++)
    types[n] = req->pgr_params[n].pgb_type;

  if (!(stmt = (struct PgStmt*) worker_alloc(sizeof(*stmt))))
    return DB_ERR_RESOURCE;

  stmt->pgs_hash = hash;
  stmt->pgs_nparams = req->pgr_nparams;
  sprintf(stmt->pgs_name, "ircu_%lu", ++conn->pgc_seq);

  if (!(stmt->pgs_sql = (char*) worker_alloc(strlen(req->pgr_sql) + 1))) {
    worker_free(stmt);
    return DB_ERR_RESOURCE;
  }
  strcpy(stmt->pgs_sql, req->pgr_sql);

  if (req->pgr_nparams) {
    stmt->pgs_types = (Oid*) worker_alloc(sizeof(Oid) * req->pgr_nparams);
    if (!stmt->pgs_types) {
      pg_stmt_free(stmt);
      return DB_ERR_RESOURCE;
    }
    memcpy(stmt->pgs_types, types, sizeof(Oid) * req->pgr_nparams);
  }

  if (!PQsendPrepare(conn->pgc_pg, stmt->pgs_name, req->pgr_sql,
                     (int) req->pgr_nparams,
                     req->pgr_nparams ? types : 0)) {
    pg_error_log("preparing a statement", PQerrorMessage(conn->pgc_pg));
    pg_stmt_free(stmt);
    return DB_ERR_CONNECT;
  }

  collected = pg_collect(conn, stopfd, deadline, &res);

  if (collected == -1) {
    pg_stmt_free(stmt);
    if (!pg_cancel(conn, stopfd))
      pg_disconnect(conn);
    return DB_ERR_TIMEOUT;
  }
  if (collected < 0 || !res) {
    pg_stmt_free(stmt);
    pg_disconnect(conn);
    return DB_ERR_CONNECT;
  }

  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    code = pg_error_from_sqlstate(PQresultErrorField(res, PG_DIAG_SQLSTATE));
    pg_error_log("preparing a statement", PQresultErrorMessage(res));
    PQclear(res);
    pg_stmt_free(stmt);
    return code;
  }

  PQclear(res);

  stmt->pgs_hnext = conn->pgc_buckets[hash % PG_STMT_BUCKETS];
  conn->pgc_buckets[hash % PG_STMT_BUCKETS] = stmt;
  conn->pgc_nstmts++;

  *stmt_out = stmt;

  return DB_OK;
}

/** Run one request on an open connection.
 *
 * @param[in,out] conn Connection to run on.
 * @param[in] req The request.
 * @param[in] stopfd worker_stop_fd(), or -1.
 * @param[in] deadline When to give up.
 * @param[out] rows Receives the result rows, or NULL.
 * @param[out] nrows Receives the row count.
 * @return #DB_OK on success.
 */
static enum DbError pg_execute(struct PgConn* conn, struct PgRequest* req,
                               int stopfd, const struct PgDeadline* deadline,
                               json_t** rows, unsigned int* nrows)
{
  const char* values[DB_MAX_PARAMS];
  int lengths[DB_MAX_PARAMS];
  int formats[DB_MAX_PARAMS];
  struct PgStmt* stmt;
  PGresult* res;
  enum DbError code;
  unsigned int n;
  int collected;

  if ((code = pg_prepare(conn, req, stopfd, deadline, &stmt)) != DB_OK)
    return code;

  for (n = 0; n < req->pgr_nparams; n++) {
    values[n] = req->pgr_params[n].pgb_value;
    lengths[n] = req->pgr_params[n].pgb_length;
    formats[n] = req->pgr_params[n].pgb_format;
  }

  /* Result format 0: the server renders the values and pg_json.c decides
   * what each one is worth as JSON.  See the note there.
   */
  if (!PQsendQueryPrepared(conn->pgc_pg, stmt->pgs_name,
                           (int) req->pgr_nparams,
                           req->pgr_nparams ? values : 0,
                           req->pgr_nparams ? lengths : 0,
                           req->pgr_nparams ? formats : 0, 0)) {
    pg_error_log("running a statement", PQerrorMessage(conn->pgc_pg));
    pg_disconnect(conn);
    return DB_ERR_CONNECT;
  }

  collected = pg_collect(conn, stopfd, deadline, &res);

  if (collected == -1) {
    if (!pg_cancel(conn, stopfd))
      pg_disconnect(conn);
    return DB_ERR_TIMEOUT;
  }
  if (collected < 0 || !res) {
    pg_disconnect(conn);
    return DB_ERR_CONNECT;
  }

  switch (PQresultStatus(res)) {
  case PGRES_TUPLES_OK:
    *rows = pg_json_rows(res);
    *nrows = (unsigned int) PQntuples(res);
    code = *rows ? DB_OK : DB_ERR_RESOURCE;
    break;

  case PGRES_COMMAND_OK: {
    /* A statement that returns nothing still has an answer: how many rows
     * it touched.  The rows themselves are an empty array, not NULL, so a
     * caller can iterate the result of anything without a special case.
     */
    const char* affected = PQcmdTuples(res);

    *rows = json_array();
    *nrows = (affected && *affected) ? (unsigned int) atoi(affected) : 0;
    code = *rows ? DB_OK : DB_ERR_RESOURCE;
    break;
  }

  default:
    code = pg_error_from_sqlstate(PQresultErrorField(res, PG_DIAG_SQLSTATE));
    pg_error_log("running a statement", PQresultErrorMessage(res));
    break;
  }

  PQclear(res);

  return code;
}

/* ------------------------------------------------------------------------
 * The thread.
 * ------------------------------------------------------------------------ */

struct PgConn* pg_conn_new(struct PgPool* pool, unsigned int index)
{
  struct PgConn* conn = (struct PgConn*) worker_alloc(sizeof(*conn));

  if (!conn)
    return 0;

  conn->pgc_pool = pool;

  snprintf(conn->pgc_name, sizeof(conn->pgc_name), "pg-%s-%u",
           pg_pool_label(pool), index);

  return conn;
}

void pg_conn_free(struct PgConn* conn)
{
  if (!conn)
    return;

  pg_disconnect(conn);
  worker_free(conn);
}

const char* pg_conn_name(const struct PgConn* conn)
{
  return conn->pgc_name;
}

void pg_conn_main(struct Worker* worker, void* arg)
{
  struct PgConn* conn = (struct PgConn*) arg;
  int stopfd = worker_stop_fd(worker);

  while (!worker_stopping(worker)) {
    struct PgRequest* req = pg_pool_take(conn->pgc_pool, worker);
    struct PgDeadline deadline;
    json_t* rows = 0;
    unsigned int nrows = 0;
    enum DbError code;

    if (!req)
      continue;

    pg_deadline_set(&deadline, req->pgr_timeout_ms);

    if ((code = pg_connect(conn, stopfd, &deadline)) == DB_OK)
      code = pg_execute(conn, req, stopfd, &deadline, &rows, &nrows);

    if (code != DB_OK && rows) {
      json_decref(rows);
      rows = 0;
      nrows = 0;
    }

    pg_pool_count(conn->pgc_pool, code);
    pg_pool_answer(worker, req->pgr_id, rows, nrows, code);
    pg_request_free(req);
  }

  /* The connection belongs to this thread and nothing else may close it,
   * so it goes now rather than in pg_conn_free(), which the main thread
   * calls once this has returned.
   */
  pg_disconnect(conn);
}
