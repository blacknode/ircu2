/*
 * IRC - Internet Relay Chat, ircd/migration_run.c
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
 * @brief Running migrations, one at a time, for somebody who asked.
 *
 * A job is a list of steps, and one step at a time goes to db_migrate() and
 * comes back through a callback.  Two things shape all of it, and both are
 * about time passing:
 *
 *   - A job copies everything it needs -- the module's name and the SQL of
 *     every step.  The module that owns them can be unloaded while its
 *     migration is still running, and a job holding a pointer into the
 *     shared object would be reading an unmapped page by the time the
 *     answer came back.
 *
 *   - A job remembers the operator by numnick, never by pointer, exactly as
 *     a worker task does.  A migration can take minutes; the operator who
 *     started it may be long gone, and a @c struct @c Client returned to the
 *     free list belongs to somebody else by then.
 *
 * Nothing here ever starts by itself.  migration_core_start() is the one
 * exception and it only ever runs the server's own set, which exists to
 * create the table everything else is recorded in.
 *
 * Validation is migration.c.  See doc/readme.migrations.
 */
#include "config.h"

#include "migration.h"

#include "client.h"
#include "db.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "s_debug.h"
#include "send.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/** Length of the numnick a job remembers its operator by, without the NUL. */
#define MIGRATION_NUMNICKLEN 5

/** The core's own migrations, embedded by the build. */
extern const struct MigrationFile ircu_core_migrations[];

/* ------------------------------------------------------------------------
 * Jobs.
 * ------------------------------------------------------------------------ */

/** One migration a job is going to run. */
struct MigrationStep {
  unsigned int ms_version;                     /**< Version to record. */
  char         ms_name[MIGRATION_NAME_LEN + 1];/**< Name to record. */
  char*        ms_sql;                         /**< A copy of the SQL. */
};

/** A sequence of migrations, running one at a time. */
struct MigrationJob {
  struct MigrationJob* mj_next;    /**< Next job on #migration_jobs. */
  char         mj_module[MIGRATION_MODULE_LEN + 1]; /**< Whose migrations. */
  char         mj_exec_by[NICKLEN + 1];  /**< Recorded in @c exec_by. */
  char         mj_nick[MIGRATION_NUMNICKLEN + 1]; /**< Operator's numnick. */
  time_t       mj_born;            /**< cli_firsttime() of that operator. */
  int          mj_revert;          /**< Non-zero when going down. */
  int          mj_core;            /**< Non-zero for the start-up job. */
  unsigned int mj_count;           /**< Steps in #mj_steps. */
  unsigned int mj_at;              /**< Step being run now. */
  unsigned int mj_done;            /**< Steps that succeeded. */
  struct MigrationStep* mj_steps;  /**< #mj_count of them, in order. */
};

/** Jobs in flight.  One per module at a time; see migration_job_running(). */
static struct MigrationJob* migration_jobs;

/** Non-zero once the core job has been started in this run of the server. */
static int migration_core_done;

/** The core's own set, built once and kept. */
static struct MigrationSet* migration_core_set;

/** The core's migrations, validated.
 * @param[out] errstr Receives the reason when this returns NULL.
 * @return The set, or NULL if the server's own migrations are malformed --
 *   which would be a bug in the server rather than in anything an operator
 *   installed.
 */
static const struct MigrationSet* migration_core_get(const char** errstr)
{
  *errstr = 0;

  if (!migration_core_set)
    migration_core_set = migration_build(MIGRATION_CORE, ircu_core_migrations,
                                         errstr);

  return migration_core_set;
}

/** Remember \a sptr on \a job by numnick, never by pointer.
 * @param[in,out] job Job to attach the operator to.
 * @param[in] cptr Operator, or NULL for the core's own job.
 */
static void migration_job_set_client(struct MigrationJob* job,
                                     struct Client* cptr)
{
  size_t len;

  if (!cptr || !cli_user(cptr)) {
    job->mj_nick[0] = '\0';
    job->mj_born = 0;
    return;
  }

  ircd_strncpy(job->mj_nick, cli_yxx(cli_user(cptr)->server),
               MIGRATION_NUMNICKLEN);
  len = strlen(job->mj_nick);
  ircd_strncpy(job->mj_nick + len, cli_yxx(cptr),
               MIGRATION_NUMNICKLEN - len);

  job->mj_born = cli_firsttime(cptr);
}

/** The operator who started \a job, if they are still here.
 * @param[in] job Job to ask about.
 * @return The client, or NULL.
 */
static struct Client* migration_job_client(const struct MigrationJob* job)
{
  struct Client* cptr;

  if (!job->mj_nick[0])
    return 0;

  if (!(cptr = findNUser(job->mj_nick)))
    return 0;

  if (cli_firsttime(cptr) != job->mj_born)
    return 0;

  return cptr;
}

/** Tell whoever is watching \a job what happened.
 *
 * The core's start-up job has no operator and goes to the log; an
 * operator's job goes to them, and to the log as well, because a schema
 * change is worth a record that outlives the session.
 * @param[in] job Job reporting.
 * @param[in] error Non-zero if this is a failure.
 * @param[in] pattern Message, printf-style, and its arguments.
 */
static void migration_report(struct MigrationJob* job, int error,
                             const char* pattern, ...)
{
  char text[512];
  struct Client* sptr;
  va_list vl;

  va_start(vl, pattern);
  ircd_vsnprintf(0, text, sizeof(text), pattern, vl);
  va_end(vl);

  log_write(LS_SYSTEM, error ? L_ERROR : L_INFO, 0, "migration: %s", text);

  if ((sptr = migration_job_client(job)))
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :%s", sptr, text);

  /* A schema change is network-visible in its consequences, so the other
   * operators hear about it whether or not they asked.
   */
  if (!job->mj_core)
    sendto_opmask_butone(sptr, SNO_OLDSNO, "%s", text);
}

/** Release a job and everything it copied.
 * @param[in] job Job to release.
 */
static void migration_job_free(struct MigrationJob* job)
{
  struct MigrationJob** job_p;
  unsigned int i;

  for (job_p = &migration_jobs; *job_p; job_p = &(*job_p)->mj_next) {
    if (*job_p == job) {
      *job_p = job->mj_next;
      break;
    }
  }

  for (i = 0; i < job->mj_count; i++)
    MyFree(job->mj_steps[i].ms_sql);

  MyFree(job->mj_steps);
  MyFree(job);
}

/** Is a job already running for \a module?
 *
 * Two operators migrating one module at once would interleave their steps
 * and record them in an order neither of them chose.
 * @param[in] module Module to look for.
 */
static int migration_job_running(const char* module)
{
  struct MigrationJob* job;

  for (job = migration_jobs; job; job = job->mj_next)
    if (0 == ircd_strcmp(job->mj_module, module))
      return 1;

  return 0;
}

/** Start a job.
 * @param[in] module Module the migrations belong to.
 * @param[in] sptr Operator, or NULL for the core.
 * @param[in] revert Non-zero to go down instead of up.
 * @param[in] count Steps it will have.
 * @return The job, already on #migration_jobs.
 */
static struct MigrationJob* migration_job_new(const char* module,
                                              struct Client* sptr,
                                              int revert, unsigned int count)
{
  struct MigrationJob* job = (struct MigrationJob*) MyCalloc(1, sizeof(*job));

  ircd_strncpy(job->mj_module, module, MIGRATION_MODULE_LEN);
  ircd_strncpy(job->mj_exec_by, sptr ? cli_name(sptr) : cli_name(&me),
               NICKLEN);
  job->mj_revert = revert;
  job->mj_core = sptr ? 0 : 1;
  job->mj_count = count;
  job->mj_steps = (struct MigrationStep*)
    MyCalloc(count ? count : 1, sizeof(struct MigrationStep));

  migration_job_set_client(job, sptr);

  job->mj_next = migration_jobs;
  migration_jobs = job;

  return job;
}

/** Fill in one step, copying the SQL out of the module.
 * @param[in,out] job Job to add to.
 * @param[in] index Which step.
 * @param[in] migration Migration to run.
 * @param[in] revert Non-zero to copy the down instead of the up.
 */
static void migration_job_step(struct MigrationJob* job, unsigned int index,
                              const struct Migration* migration, int revert)
{
  struct MigrationStep* step = &job->mj_steps[index];
  const char* sql = revert ? migration->mg_down : migration->mg_up;

  step->ms_version = migration->mg_version;
  ircd_strncpy(step->ms_name, migration->mg_name, MIGRATION_NAME_LEN);
  DupString(step->ms_sql, sql);
}

static void migration_run_step(struct MigrationJob* job);

/** One step finished.  Main thread.
 * @param[in] res What the database said.
 * @param[in] user The job.
 */
static void migration_step_done(const struct DbResult* res, void* user)
{
  struct MigrationJob* job = (struct MigrationJob*) user;
  struct MigrationStep* step = &job->mj_steps[job->mj_at];

  if (res->err.dberr_code != DB_OK) {
    migration_report(job, 1,
                     "%s v%u_%s (%s) failed: %s -- %u of %u applied",
                     job->mj_module, step->ms_version, step->ms_name,
                     job->mj_revert ? "revert" : "apply",
                     res->err.dberr_message, job->mj_done, job->mj_count);
    migration_job_free(job);
    return;
  }

  job->mj_done++;

  /* The driver hands back the row it recorded, and the part of it worth an
   * operator's attention is how long the SQL took.
   */
  migration_report(job, 0, "%s v%u_%s %s in %s",
                   job->mj_module, step->ms_version, step->ms_name,
                   job->mj_revert ? "reverted" : "applied",
                   res->rows ? db_row_str(res->data, 0, "exec_duration")
                             : "an unrecorded time");

  job->mj_at++;

  if (job->mj_at >= job->mj_count) {
    migration_report(job, 0, "%s: %u migration%s %s",
                     job->mj_module, job->mj_done,
                     job->mj_done == 1 ? "" : "s",
                     job->mj_revert ? "reverted" : "applied");
    migration_job_free(job);
    return;
  }

  migration_run_step(job);
}

/** Send the current step to the driver.
 * @param[in] job Job to advance.
 */
static void migration_run_step(struct MigrationJob* job)
{
  struct MigrationStep* step = &job->mj_steps[job->mj_at];
  struct DbMigration migration;
  enum DbError err;

  memset(&migration, 0, sizeof(migration));
  migration.dbm_module = job->mj_module;
  migration.dbm_version = step->ms_version;
  migration.dbm_name = step->ms_name;
  migration.dbm_sql = step->ms_sql;
  migration.dbm_exec_by = job->mj_exec_by;
  migration.dbm_revert = job->mj_revert;

  if ((err = db_migrate(0, &migration, migration_step_done, job)) != DB_OK) {
    migration_report(job, 1, "%s v%u_%s could not be started: %s",
                     job->mj_module, step->ms_version, step->ms_name,
                     db_strerror(err));
    migration_job_free(job);
  }
}

/* ------------------------------------------------------------------------
 * Reading what is applied.
 * ------------------------------------------------------------------------ */

/** What to do once the applied versions are known. */
enum MigrationWant {
  MIGRATION_WANT_STATUS,   /**< Report declared against applied. */
  MIGRATION_WANT_APPLY,    /**< Apply what is missing. */
  MIGRATION_WANT_REVERT    /**< Revert what is there. */
};

/** A question asked of the database, and who asked it. */
struct MigrationQuery {
  char               mq_module[MIGRATION_MODULE_LEN + 1]; /**< Module. */
  char               mq_nick[MIGRATION_NUMNICKLEN + 1];   /**< Operator. */
  time_t             mq_born;      /**< cli_firsttime() of the operator. */
  enum MigrationWant mq_want;      /**< What the answer is for. */
  unsigned int       mq_bound;     /**< APPLY's upto, REVERT's downto. */
  int                mq_core;      /**< The start-up job asked. */
};

/** The operator who asked \a query, if they are still here. */
static struct Client* migration_query_client(const struct MigrationQuery* q)
{
  struct Client* cptr;

  if (!q->mq_nick[0])
    return 0;
  if (!(cptr = findNUser(q->mq_nick)))
    return 0;
  if (cli_firsttime(cptr) != q->mq_born)
    return 0;

  return cptr;
}

/** Tell the operator behind \a query something, if they are still here. */
static void migration_query_reply(const struct MigrationQuery* q,
                                  const char* pattern, ...)
{
  struct Client* sptr = migration_query_client(q);
  char text[512];
  va_list vl;

  if (!sptr)
    return;

  va_start(vl, pattern);
  ircd_vsnprintf(0, text, sizeof(text), pattern, vl);
  va_end(vl);

  sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :%s", sptr, text);
}

/** Non-zero if \a version appears in the rows \a applied.
 *
 * The rows are jansson values and the ircd does not link against jansson,
 * so they are read through db_rows() and db_row_int(), which ask the driver.
 * See db.h.
 * @param[in] applied Rows from the migrations table.
 * @param[in] version Version to look for.
 */
static int migration_is_applied(struct json_t* applied, unsigned int version)
{
  unsigned int i;
  unsigned int rows = db_rows(applied);

  for (i = 0; i < rows; i++)
    if ((unsigned int) db_row_int(applied, i, "version") == version)
      return 1;

  return 0;
}

/** Build and start the job the answer calls for.
 * @param[in] res The applied versions.
 * @param[in] user The #MigrationQuery.
 */
static void migration_applied_known(const struct DbResult* res, void* user)
{
  struct MigrationQuery* query = (struct MigrationQuery*) user;
  const struct MigrationSet* set = 0;
  struct ModuleHandle* mod;
  struct MigrationJob* job = 0;
  struct Client* sptr = migration_query_client(query);
  unsigned int count = 0;
  unsigned int i;

  /* The module may have been unloaded while the question was in flight;
   * everything below needs its declared set, so there is nothing to do.
   */
  if (query->mq_core) {
    const char* err = 0;

    set = migration_core_get(&err);
  } else if ((mod = module_find(query->mq_module)))
    set = module_migrations(mod);

  if (!set) {
    migration_query_reply(query, "Module %s is no longer loaded",
                          query->mq_module);
    MyFree(query);
    return;
  }

  /* A missing migrations table is not a failure when the core is the one
   * asking: it is the state the core's own v1 exists to fix.
   */
  if (res->err.dberr_code != DB_OK) {
    if (query->mq_core && res->err.dberr_code == DB_ERR_UNDEFINED) {
      /* Nothing applied; fall through with an empty answer. */
    } else {
      if (res->err.dberr_code == DB_ERR_UNDEFINED)
        migration_query_reply(query,
                              "The migrations table does not exist yet; the "
                              "server creates it at start-up once a database "
                              "driver is loaded and configured");
      else
        migration_query_reply(query, "Could not read the migrations table: %s",
                              res->err.dberr_message);

      if (query->mq_core)
        log_write(LS_SYSTEM, L_ERROR, 0,
                  "migration: could not read the migrations table: %s",
                  res->err.dberr_message);
      MyFree(query);
      return;
    }
  }

  switch (query->mq_want) {
  case MIGRATION_WANT_STATUS:
    migration_query_reply(query, "Migrations for %s:", query->mq_module);

    for (i = 0; i < set->ms_count; i++) {
      const struct Migration* migration = &set->ms_list[i];
      int applied = migration_is_applied(res->data, migration->mg_version);

      migration_query_reply(query, "  v%u_%s  %s", migration->mg_version,
                            migration->mg_name,
                            applied ? "applied" : "pending");
    }

    migration_query_reply(query, "End of migrations for %s (%u declared)",
                          query->mq_module, set->ms_count);
    MyFree(query);
    return;

  case MIGRATION_WANT_APPLY:
    for (i = 0; i < set->ms_count; i++) {
      const struct Migration* migration = &set->ms_list[i];

      if (query->mq_bound && migration->mg_version > query->mq_bound)
        break;
      if (!migration_is_applied(res->data, migration->mg_version))
        count++;
    }

    if (!count) {
      migration_query_reply(query, "Nothing to apply for %s",
                            query->mq_module);
      if (query->mq_core)
        log_write(LS_SYSTEM, L_INFO, 0,
                  "migration: the core schema is up to date");
      MyFree(query);
      return;
    }

    job = migration_job_new(query->mq_module, sptr, 0, count);
    if (query->mq_core)
      job->mj_core = 1;

    count = 0;
    for (i = 0; i < set->ms_count; i++) {
      const struct Migration* migration = &set->ms_list[i];

      if (query->mq_bound && migration->mg_version > query->mq_bound)
        break;
      if (!migration_is_applied(res->data, migration->mg_version))
        migration_job_step(job, count++, migration, 0);
    }
    break;

  case MIGRATION_WANT_REVERT:
  default: {
    unsigned int newest = 0;
    unsigned int lowest;

    /* The newest applied version is where a revert starts. */
    for (i = set->ms_count; i > 0; i--)
      if (migration_is_applied(res->data, set->ms_list[i - 1].mg_version)) {
        newest = set->ms_list[i - 1].mg_version;
        break;
      }

    if (!newest) {
      migration_query_reply(query, "Nothing to revert for %s",
                            query->mq_module);
      MyFree(query);
      return;
    }

    /* Without a bound that is also where it stops: going down destroys
     * whatever the migration built, so one step is the default and "all of
     * them" has to be asked for.
     */
    lowest = query->mq_bound ? query->mq_bound : newest;

    for (i = newest; i >= lowest; i--)
      if (migration_is_applied(res->data, i))
        count++;

    if (!count) {
      migration_query_reply(query, "Nothing to revert for %s",
                            query->mq_module);
      MyFree(query);
      return;
    }

    job = migration_job_new(query->mq_module, sptr, 1, count);

    count = 0;
    for (i = newest; i >= lowest; i--)
      if (migration_is_applied(res->data, i))
        migration_job_step(job, count++, &set->ms_list[i - 1], 1);
    break;
  }
  }

  MyFree(query);

  migration_report(job, 0, "%s: %s %u migration%s",
                   job->mj_module,
                   job->mj_revert ? "reverting" : "applying",
                   job->mj_count, job->mj_count == 1 ? "" : "s");

  migration_run_step(job);
}

/** Ask which of \a module's migrations are applied, then do \a want.
 * @param[in] sptr Operator, or NULL for the core.
 * @param[in] module Module to ask about.
 * @param[in] want What the answer is for.
 * @param[in] bound APPLY's upto or REVERT's downto; 0 for the default.
 * @return Non-zero when the question was accepted.
 */
static int migration_ask_applied(struct Client* sptr, const char* module,
                                 enum MigrationWant want, unsigned int bound)
{
  struct MigrationQuery* query;
  struct DbParam name = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam* params[2];
  struct DbQuery sql;
  enum DbError err;

  query = (struct MigrationQuery*) MyCalloc(1, sizeof(*query));
  ircd_strncpy(query->mq_module, module, MIGRATION_MODULE_LEN);
  query->mq_want = want;
  query->mq_bound = bound;
  query->mq_core = sptr ? 0 : 1;

  if (sptr) {
    size_t len;

    ircd_strncpy(query->mq_nick, cli_yxx(cli_user(sptr)->server),
                 MIGRATION_NUMNICKLEN);
    len = strlen(query->mq_nick);
    ircd_strncpy(query->mq_nick + len, cli_yxx(sptr),
                 MIGRATION_NUMNICKLEN - len);
    query->mq_born = cli_firsttime(sptr);
  }

  name.value = query->mq_module;
  params[0] = &name;
  params[1] = 0;

  sql.sql = "select version from migrations where module_name = $1"
            " order by version";
  sql.params = params;

  err = db_query(0, &sql, migration_applied_known, query);

  if (err != DB_OK) {
    if (sptr)
      sendcmdto_one(&me, CMD_NOTICE, sptr,
                    "%C :Cannot reach the database: %s", sptr,
                    db_strerror(err));
    else
      log_write(LS_SYSTEM, L_ERROR, 0,
                "migration: cannot reach the database: %s", db_strerror(err));
    MyFree(query);
    return 0;
  }

  return 1;
}

/* ------------------------------------------------------------------------
 * The core's own schema.
 * ------------------------------------------------------------------------ */

void migration_core_start(void)
{
  const char* err = 0;
  struct MigrationSet* set;

  if (migration_core_done || !db_available())
    return;

  /* The core set goes through the same validator as any module's.  It
   * should never fail, which is exactly why it is worth checking: a broken
   * one would otherwise be found by the first server that started with an
   * empty database.
   */
  if (!(set = migration_build(MIGRATION_CORE, ircu_core_migrations, &err))) {
    if (err)
      log_write(LS_SYSTEM, L_ERROR, 0,
                "migration: the server's own migrations are broken: %s", err);
    return;
  }
  migration_free(set);

  migration_core_done = 1;

  log_write(LS_SYSTEM, L_INFO, 0,
            "migration: bringing the core schema up to date");

  migration_ask_applied(0, MIGRATION_CORE, MIGRATION_WANT_APPLY, 0);
}

/* ------------------------------------------------------------------------
 * The operator commands.
 * ------------------------------------------------------------------------ */

/** Render the migrations table for the operator who asked.
 * @param[in] res Rows from the table.
 * @param[in] user The #MigrationQuery that asked.
 */
static void migration_list_done(const struct DbResult* res, void* user)
{
  struct MigrationQuery* query = (struct MigrationQuery*) user;
  unsigned int rows;
  unsigned int i;

  if (res->err.dberr_code != DB_OK) {
    if (res->err.dberr_code == DB_ERR_UNDEFINED)
      migration_query_reply(query,
                            "The migrations table does not exist yet; no "
                            "migration has ever been applied here");
    else
      migration_query_reply(query, "Could not read the migrations table: %s",
                            res->err.dberr_message);
    MyFree(query);
    return;
  }

  rows = db_rows(res->data);

  migration_query_reply(query,
                        "%-16s %-6s %-28s %-10s %-12s %s",
                        "Module", "Ver", "Migration", "Duration", "By",
                        "Applied");

  for (i = 0; i < rows; i++)
    migration_query_reply(query, "%-16s v%-5u %-28s %-10s %-12s %s",
                          db_row_str(res->data, i, "module_name"),
                          (unsigned int) db_row_int(res->data, i, "version"),
                          db_row_str(res->data, i, "module_migration_name"),
                          db_row_str(res->data, i, "exec_duration"),
                          db_row_str(res->data, i, "exec_by"),
                          db_row_str(res->data, i, "created_at"));

  migration_query_reply(query, "End of migration list (%u row%s)", rows,
                        rows == 1 ? "" : "s");

  MyFree(query);
}

void migration_cmd_list(struct Client* sptr)
{
  struct MigrationQuery* query;
  struct DbQuery sql;
  enum DbError err;
  size_t len;

  query = (struct MigrationQuery*) MyCalloc(1, sizeof(*query));

  ircd_strncpy(query->mq_nick, cli_yxx(cli_user(sptr)->server),
               MIGRATION_NUMNICKLEN);
  len = strlen(query->mq_nick);
  ircd_strncpy(query->mq_nick + len, cli_yxx(sptr),
               MIGRATION_NUMNICKLEN - len);
  query->mq_born = cli_firsttime(sptr);

  sql.sql = "select version, module_name, module_migration_name,"
            " exec_duration, exec_by, created_at from migrations"
            " order by module_name, version";
  sql.params = 0;

  if ((err = db_query(0, &sql, migration_list_done, query)) != DB_OK) {
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Cannot reach the database: %s",
                  sptr, db_strerror(err));
    MyFree(query);
  }
}

void migration_cmd_status(struct Client* sptr, struct ModuleHandle* mod)
{
  if (!module_migrations(mod)) {
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Module %s ships no migrations",
                  sptr, module_name(mod));
    return;
  }

  migration_ask_applied(sptr, module_name(mod), MIGRATION_WANT_STATUS, 0);
}

/** Shared front half of APPLY and REVERT.
 * @param[in] sptr Operator asking.
 * @param[in] mod Module to migrate.
 * @param[in] revert Non-zero for REVERT.
 * @param[in] bound Version bound, or 0.
 */
static void migration_cmd_run(struct Client* sptr, struct ModuleHandle* mod,
                              int revert, unsigned int bound)
{
  const struct MigrationSet* set = module_migrations(mod);

  if (!set) {
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Module %s ships no migrations",
                  sptr, module_name(mod));
    return;
  }

  if (bound > set->ms_count) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  "%C :Module %s has no v%u; it declares v1 to v%u", sptr,
                  module_name(mod), bound, set->ms_count);
    return;
  }

  if (migration_job_running(module_name(mod))) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  "%C :A migration of %s is already running; wait for it",
                  sptr, module_name(mod));
    return;
  }

  migration_ask_applied(sptr, module_name(mod),
                        revert ? MIGRATION_WANT_REVERT : MIGRATION_WANT_APPLY,
                        bound);
}

void migration_cmd_apply(struct Client* sptr, struct ModuleHandle* mod,
                         unsigned int upto)
{
  migration_cmd_run(sptr, mod, 0, upto);
}

void migration_cmd_revert(struct Client* sptr, struct ModuleHandle* mod,
                          unsigned int downto)
{
  migration_cmd_run(sptr, mod, 1, downto);
}

void migration_shutdown(void)
{
  while (migration_jobs)
    migration_job_free(migration_jobs);

  migration_free(migration_core_set);
  migration_core_set = 0;
}
