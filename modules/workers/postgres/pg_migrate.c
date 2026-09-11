/*
 * IRC - Internet Relay Chat, modules/workers/postgres/pg_migrate.c
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
 * @brief Running a migration: its own connection, its own transaction.
 *
 * A migration is not a query, and almost nothing about the rest of this
 * driver fits it.  Three differences decide the shape of this file.
 *
 * @section mig_ddl It is DDL, so it is not prepared
 *
 * @c PREPARE in PostgreSQL accepts @c SELECT, @c INSERT, @c UPDATE,
 * @c DELETE, @c VALUES and @c MERGE.  It does not accept @c CREATE @c TABLE,
 * @c ALTER @c TABLE or @c CREATE @c INDEX, which is what a migration is made
 * of -- and a migration is a script of several statements besides, where a
 * prepared statement is exactly one.  So the script goes out through
 * @c PQsendQuery, the one place in this driver that sends SQL as a string.
 *
 * That is safe here for a reason that does not generalise: the string is a
 * file the module author shipped, embedded in the shared object at build
 * time.  It is not a query anybody assembled, it takes no parameters, and
 * nothing a user ever typed can reach it.  The row that records the
 * migration is a different matter -- it carries the operator's nick -- and
 * that one is bound through @c PQexecPrepared like everything else.
 *
 * @section mig_tx It is all-or-nothing
 *
 * The script and the row saying it ran are one explicit transaction.  A
 * migration that fails half way leaves neither the change nor the record; a
 * recorded migration is one that actually happened.  Without that, a crash
 * between the two would leave the table claiming a migration that is not
 * there, or hiding one that is -- and the next operator would apply it
 * twice or never.
 *
 * @section mig_conn It takes as long as it takes
 *
 * A pooled connection has a five second deadline and shares its thread with
 * every query behind it.  An @c ALTER @c TABLE on a real table takes
 * minutes.  So a migration gets a connection of its own, opened for it and
 * closed after it, on a pool worker task rather than a pool connection
 * thread -- one thread of the general pool is occupied for the duration, and
 * no query is delayed by it.
 *
 * Everything from pg_migrate_work() to the end of that section runs in a
 * worker thread and obeys worker.h: worker_alloc(), worker_log(), and
 * nothing of the core at all.
 */
#include "postgres.h"

#include "module.h"

#include <stdio.h>
#include <string.h>

/** What the main thread hands the worker.  Worker memory, copied. */
struct PgMigrateJob {
  unsigned long pgm_id;         /**< Handle for db_complete(). */
  int           pgm_timeout_ms; /**< Deadline for the whole thing. */
  int           pgm_revert;     /**< Non-zero to revert. */
  unsigned int  pgm_version;    /**< Version being applied or reverted. */
  char*         pgm_dsn;        /**< Where to connect. */
  char*         pgm_module;     /**< Module the migration belongs to. */
  char*         pgm_name;       /**< Migration name. */
  char*         pgm_sql;        /**< The script. */
  char*         pgm_exec_by;    /**< Operator's nick. */
};

/** What the worker hands back. */
struct PgMigrateResult {
  unsigned long pgm_id;                  /**< Handle for db_complete(). */
  json_t*       pgm_rows;                /**< The recorded row, or NULL. */
  enum DbError  pgm_code;                /**< How it ended. */
};

/* ------------------------------------------------------------------------
 * The worker thread.
 * ------------------------------------------------------------------------ */

/** Render \a ms as something an operator can read.
 *
 * "840ms", "3.2s", "4m 11s".  Goes in @c exec_duration, which is text for
 * exactly this reason: the number of milliseconds is of no use to anybody
 * reading @c /MODULE @c MIGRATION @c LIST six months later.
 * @param[in] ms Milliseconds the script took.
 * @param[out] out Receives the text.
 * @param[in] size Size of \a out.
 */
static void pg_migrate_duration(long ms, char* out, size_t size)
{
  if (ms < 1000)
    snprintf(out, size, "%ldms", ms);
  else if (ms < 60000)
    snprintf(out, size, "%ld.%lds", ms / 1000, (ms % 1000) / 100);
  else
    snprintf(out, size, "%ldm %lds", ms / 60000, (ms % 60000) / 1000);
}

/** Send one statement as a string and wait for it.  Worker thread.
 *
 * The transaction control and the migration script itself; see @ref mig_ddl
 * for why these do not go through the prepared path.
 * @param[in] pg Connection.
 * @param[in] deadline When to give up.
 * @param[in] sql Statement or script.
 * @param[in] what What this is, for the log.
 * @param[out] code Receives the error when this returns zero.
 * @return Non-zero on success.
 */
static int pg_migrate_send(PGconn* pg, const struct PgDeadline* deadline,
                           const char* sql, const char* what,
                           enum DbError* code)
{
  PGresult* res;
  ExecStatusType status;
  int collected;

  if (!PQsendQuery(pg, sql)) {
    pg_error_log(what, PQerrorMessage(pg));
    *code = DB_ERR_CONNECT;
    return 0;
  }

  collected = pg_collect(pg, -1, deadline, &res);

  if (collected == -1) {
    *code = DB_ERR_TIMEOUT;
    return 0;
  }
  if (collected < 0 || !res) {
    *code = DB_ERR_CONNECT;
    return 0;
  }

  status = PQresultStatus(res);

  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    *code = pg_error_from_sqlstate(PQresultErrorField(res, PG_DIAG_SQLSTATE));
    pg_error_log(what, PQresultErrorMessage(res));
    PQclear(res);
    return 0;
  }

  PQclear(res);

  return 1;
}

/** Write or delete the row that records the migration.  Worker thread.
 *
 * Bound parameters, prepared statement: this one carries an operator's nick
 * and a module's name, so it goes the same way every other write in this
 * driver goes.  @c RETURNING hands back the row that was recorded, which is
 * what the operator is shown.
 *
 * @param[in] pg Connection, inside the transaction.
 * @param[in] job The migration.
 * @param[in] duration How long the script took, already rendered.
 * @param[in] deadline When to give up.
 * @param[out] rows Receives the recorded row.
 * @param[out] code Receives the error when this returns zero.
 * @return Non-zero on success.
 */
static int pg_migrate_record(PGconn* pg, const struct PgMigrateJob* job,
                             const char* duration,
                             const struct PgDeadline* deadline,
                             json_t** rows, enum DbError* code)
{
  static const char* const columns =
    " returning version, module_name, module_migration_name,"
    " exec_duration, exec_by, created_at";
  const char* values[5];
  char version[16];
  PGresult* res;
  const char* sql;
  int nparams;
  int collected;

  snprintf(version, sizeof(version), "%u", job->pgm_version);

  if (job->pgm_revert) {
    /* Reverting removes the record entirely: what is in the table is what
     * is applied, and a reverted migration is not.
     */
    sql = "delete from migrations where module_name = $1 and version = $2";
    values[0] = job->pgm_module;
    values[1] = version;
    nparams = 2;
  } else {
    sql = "insert into migrations (version, module_name,"
          " module_migration_name, exec_duration, exec_by)"
          " values ($1, $2, $3, $4, $5)";
    values[0] = version;
    values[1] = job->pgm_module;
    values[2] = job->pgm_name;
    values[3] = duration;
    values[4] = job->pgm_exec_by;
    nparams = 5;
  }

  {
    char statement[1024];

    snprintf(statement, sizeof(statement), "%s%s", sql, columns);

    if (!PQsendPrepare(pg, "ircu_migration_record", statement, nparams, 0)) {
      pg_error_log("preparing the migration record", PQerrorMessage(pg));
      *code = DB_ERR_CONNECT;
      return 0;
    }
  }

  if (pg_collect(pg, -1, deadline, &res) || !res) {
    *code = DB_ERR_CONNECT;
    return 0;
  }

  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    *code = pg_error_from_sqlstate(PQresultErrorField(res, PG_DIAG_SQLSTATE));
    pg_error_log("preparing the migration record", PQresultErrorMessage(res));
    PQclear(res);
    return 0;
  }
  PQclear(res);

  if (!PQsendQueryPrepared(pg, "ircu_migration_record", nparams, values,
                           0, 0, 0)) {
    pg_error_log("recording the migration", PQerrorMessage(pg));
    *code = DB_ERR_CONNECT;
    return 0;
  }

  collected = pg_collect(pg, -1, deadline, &res);

  if (collected == -1) {
    *code = DB_ERR_TIMEOUT;
    return 0;
  }
  if (collected < 0 || !res) {
    *code = DB_ERR_CONNECT;
    return 0;
  }

  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    *code = pg_error_from_sqlstate(PQresultErrorField(res, PG_DIAG_SQLSTATE));
    pg_error_log("recording the migration", PQresultErrorMessage(res));
    PQclear(res);
    return 0;
  }

  /* Reverting a migration that was not recorded is not an error worth
   * failing on -- the operator asked for a state, and that is the state --
   * but it is worth not claiming a row that does not exist.
   */
  *rows = pg_json_rows(res);
  PQclear(res);

  if (!*rows) {
    *code = DB_ERR_RESOURCE;
    return 0;
  }

  return 1;
}

/** Open a connection for one migration.  Worker thread.
 *
 * Its own connection, not one of the pool's: see @ref mig_conn.
 * @param[in] job The migration.
 * @param[in] deadline When to give up.
 * @param[out] code Receives the error when this returns NULL.
 * @return The connection, or NULL.
 */
static PGconn* pg_migrate_open(const struct PgMigrateJob* job,
                               const struct PgDeadline* deadline,
                               enum DbError* code)
{
  PostgresPollingStatusType polling = PGRES_POLLING_WRITING;
  PGconn* pg;

  if (!(pg = PQconnectStart(job->pgm_dsn))) {
    *code = DB_ERR_RESOURCE;
    return 0;
  }

  if (PQstatus(pg) == CONNECTION_BAD) {
    pg_error_log("connecting for a migration", PQerrorMessage(pg));
    PQfinish(pg);
    *code = DB_ERR_CONNECT;
    return 0;
  }

  for (;;) {
    int ready = pg_socket_wait(PQsocket(pg), polling == PGRES_POLLING_WRITING,
                               -1, deadline);

    if (ready <= 0) {
      PQfinish(pg);
      *code = ready == 0 ? DB_ERR_TIMEOUT : DB_ERR_CONNECT;
      return 0;
    }

    polling = PQconnectPoll(pg);

    if (polling == PGRES_POLLING_OK)
      break;

    if (polling == PGRES_POLLING_FAILED) {
      pg_error_log("connecting for a migration", PQerrorMessage(pg));
      PQfinish(pg);
      *code = DB_ERR_CONNECT;
      return 0;
    }
  }

  if (PQsetnonblocking(pg, 1)) {
    pg_error_log("connecting for a migration", PQerrorMessage(pg));
    PQfinish(pg);
    *code = DB_ERR_RESOURCE;
    return 0;
  }

  return pg;
}

/** Run one migration.  Worker thread.
 * @param[in,out] task The task carrying a #PgMigrateJob.
 */
static void pg_migrate_work(struct WorkTask* task)
{
  struct PgMigrateJob* job = (struct PgMigrateJob*) task->wt_in;
  struct PgMigrateResult* out;
  struct PgDeadline deadline;
  char duration[32];
  char timeout[32];
  enum DbError code = DB_ERR_INTERNAL;
  json_t* rows = 0;
  PGconn* pg;
  long elapsed;

  if (!(out = (struct PgMigrateResult*) worker_alloc(sizeof(*out)))) {
    task->wt_status = -1;
    return;
  }

  out->pgm_id = job->pgm_id;
  out->pgm_code = DB_ERR_RESOURCE;

  pg_deadline_set(&deadline, job->pgm_timeout_ms);

  if (!(pg = pg_migrate_open(job, &deadline, &code))) {
    out->pgm_code = code;
    task->wt_out = out;
    task->wt_out_len = sizeof(*out);
    return;
  }

  /* The server gets the same deadline, so that a statement blocked on a
   * lock ends on its own even if the cancel never lands.
   */
  {
    /* Failing is survivable -- the client-side deadline still holds -- so
     * this does not take the migration down with it.
     */
    enum DbError armed = DB_OK;

    snprintf(timeout, sizeof(timeout), "set statement_timeout = %d",
             job->pgm_timeout_ms);
    pg_migrate_send(pg, &deadline, timeout, "arming the migration timeout",
                    &armed);
  }

  do {
    if (!pg_migrate_send(pg, &deadline, "begin", "beginning the migration",
                         &code))
      break;

    /* The clock the operator is told about covers the script and nothing
     * else: the connect and the bookkeeping are the driver's overhead, not
     * the migration's cost.
     */
    elapsed = pg_monotonic_ms();

    if (!pg_migrate_send(pg, &deadline, job->pgm_sql,
                         job->pgm_revert ? "reverting a migration"
                                         : "applying a migration", &code))
      break;

    elapsed = pg_monotonic_ms() - elapsed;
    pg_migrate_duration(elapsed, duration, sizeof(duration));

    if (!pg_migrate_record(pg, job, duration, &deadline, &rows, &code))
      break;

    if (!pg_migrate_send(pg, &deadline, "commit", "committing the migration",
                         &code))
      break;

    code = DB_OK;
  } while (0);

  if (code != DB_OK) {
    enum DbError ignored = DB_OK;

    /* Best effort: the connection may already be unusable, in which case
     * closing it rolls back just as well.
     */
    pg_migrate_send(pg, &deadline, "rollback", "rolling back the migration",
                    &ignored);

    if (rows) {
      json_decref(rows);
      rows = 0;
    }
  }

  PQfinish(pg);

  out->pgm_code = code;
  out->pgm_rows = rows;

  task->wt_out = out;
  task->wt_out_len = sizeof(*out);
  task->wt_status = code == DB_OK ? 0 : -1;
}

/* ------------------------------------------------------------------------
 * Back in the main thread.
 * ------------------------------------------------------------------------ */

/** Release a migration task's payload.  Main thread; runs on every path.
 * @param[in] task Task to release.
 */
static void pg_migrate_free(struct WorkTask* task)
{
  struct PgMigrateJob* job = (struct PgMigrateJob*) task->wt_in;
  struct PgMigrateResult* out = (struct PgMigrateResult*) task->wt_out;

  if (job) {
    worker_free(job->pgm_dsn);
    worker_free(job->pgm_module);
    worker_free(job->pgm_name);
    worker_free(job->pgm_sql);
    worker_free(job->pgm_exec_by);
    worker_free(job);
    task->wt_in = 0;
  }

  if (out) {
    /* Non-NULL only when the answer never reached db_complete() -- the
     * module was unloaded while the migration ran, say.
     */
    if (out->pgm_rows)
      json_decref(out->pgm_rows);
    worker_free(out);
    task->wt_out = 0;
  }
}

/** Deliver the answer.  Main thread.
 * @param[in] task The finished task.
 */
static void pg_migrate_done(struct WorkTask* task)
{
  struct PgMigrateJob* job = (struct PgMigrateJob*) task->wt_in;
  struct PgMigrateResult* out = (struct PgMigrateResult*) task->wt_out;
  json_t* rows;

  if (!out) {
    if (job)
      db_complete(job->pgm_id, 0, 0, DB_ERR_RESOURCE, 0);
    return;
  }

  rows = out->pgm_rows;
  out->pgm_rows = 0;

  db_complete(out->pgm_id, rows, rows ? pg_json_count(rows) : 0,
              out->pgm_code, pg_error_message(out->pgm_code));
}

/** Copy a string into worker memory.
 * @param[in] text Text to copy, or NULL.
 * @return The copy, or NULL.
 */
static char* pg_migrate_dup(const char* text)
{
  char* copy;
  size_t len;

  if (!text)
    text = "";

  len = strlen(text);

  if ((copy = (char*) worker_alloc(len + 1)))
    memcpy(copy, text, len + 1);

  return copy;
}

enum DbError pg_migrate_submit(unsigned long id,
                               const struct DbMigration* migration)
{
  struct PgMigrateJob* job;
  struct WorkTask* task;
  const char* dsn;

  /* A migration writes, so it goes where writes go. */
  if (!(dsn = db_conf_dsn(DB_ROLE_WRITE)) || !*dsn)
    return DB_ERR_CONFIG;

  if (!(task = worker_task_new(pg_migrate_work, pg_migrate_done)))
    return DB_ERR_RESOURCE;

  task->wt_free = pg_migrate_free;

  if (!(job = (struct PgMigrateJob*) worker_alloc(sizeof(*job)))) {
    worker_task_free(task);
    return DB_ERR_RESOURCE;
  }

  task->wt_in = job;
  task->wt_in_len = sizeof(*job);

  job->pgm_id = id;
  job->pgm_timeout_ms = db_conf_migration_timeout();
  job->pgm_revert = migration->dbm_revert;
  job->pgm_version = migration->dbm_version;
  job->pgm_dsn = pg_migrate_dup(dsn);
  job->pgm_module = pg_migrate_dup(migration->dbm_module);
  job->pgm_name = pg_migrate_dup(migration->dbm_name);
  job->pgm_sql = pg_migrate_dup(migration->dbm_sql);
  job->pgm_exec_by = pg_migrate_dup(migration->dbm_exec_by);

  if (!job->pgm_dsn || !job->pgm_module || !job->pgm_name || !job->pgm_sql
      || !job->pgm_exec_by) {
    worker_task_free(task);
    return DB_ERR_RESOURCE;
  }

  /* The general worker pool, not a pool connection thread: a migration can
   * take minutes, and no query should queue behind it.
   */
  if (!module_submit_work(pg_module, task)) {
    worker_task_free(task);
    return DB_ERR_UNAVAILABLE;
  }

  return DB_OK;
}
