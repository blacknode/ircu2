/*
 * IRC - Internet Relay Chat, modules/workers/postgres/postgres.c
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
 * @brief The PostgreSQL driver: what the server sees of it.
 *
 * This module is the implementation behind db.h.  It registers itself as
 * the database driver, and from then on every db_query() and db_exec() in
 * the server -- from any module, without any of them linking against libpq
 * or knowing that it exists -- lands in pg_driver_submit() below, is run on
 * a pooled connection in a worker thread, and comes back as JSON.
 *
 * Load it like any other module:
 *
 * @code
 *   Database {
 *     dsn = "host=db.example.org dbname=ircu user=ircu";
 *     read = "host=replica.example.org dbname=ircu user=ircu";
 *     pool = 4;
 *     timeout = 2 seconds;
 *   };
 *
 *   Module { name = "postgres"; };
 * @endcode
 *
 * and set @c WORKER_THREADS to at least the size of the pools, because a
 * driver that cannot have a thread has nowhere to wait.
 *
 * The files beside this one are the parts: pg_pool.c is the queue and the
 * copying, pg_conn.c is one connection and its deadline, pg_json.c turns a
 * result into JSON, pg_error.c turns a failure into a #DbError, and
 * pg_types.c is the type map.  postgres.h says which thread each of them
 * runs in.
 */
#include "postgres.h"

#include "ircd_features.h"
#include "ircd_log.h"
#include "module.h"

struct ModuleHandle* pg_module;

/** Start one query.  Main thread.
 *
 * @param[in] id Handle to complete the query with.
 * @param[in] query Statement and parameters, the caller's to keep.
 * @param[in] role Which pool to run it on.
 * @return #DB_OK when the query was queued.
 */
static enum DbError pg_driver_submit(unsigned long id,
                                     const struct DbQuery* query,
                                     enum DbRole role)
{
  enum DbError err = DB_ERR_INTERNAL;
  struct PgPool* pool;

  if (!(pool = pg_pools_get(role, &err)))
    return err;

  return pg_pools_submit(pool, id, query);
}

/** Release a result the driver produced.  Main thread.
 *
 * The core cannot do this itself: it does not link against jansson, which
 * is the whole reason #DbDriver has this entry.
 * @param[in] data Value to release, or NULL.
 */
static void pg_driver_release(struct json_t* data)
{
  if (data)
    json_decref((json_t*) data);
}

/** What the server asks of this driver. */
static const struct DbDriver pg_driver = {
  "postgres",
  pg_driver_submit,
  pg_driver_release
};

/** Register the driver.
 * @param[in] mod Handle for this module.
 * @return Zero to stay loaded.
 */
static int pg_init(struct ModuleHandle* mod)
{
  pg_module = mod;

  if (!db_register_driver(mod, &pg_driver))
    return -1;

  /* The pools are not started here.  mi_init runs while ircd.conf is still
   * being read, so the Database{} block may not have been seen yet -- and
   * the worker subsystem is not up either.  pg_pools_get() builds them the
   * first time a query needs one; see pg_pool.c.
   *
   * Both of the things that could still be wrong are worth saying now
   * rather than once per failed query.
   */
  if (!feature_int(FEAT_WORKER_THREADS))
    log_write(LS_SYSTEM, L_WARNING, 0,
              "postgres: loaded, but WORKER_THREADS is 0, so every query "
              "will be refused until it is set");

  log_write(LS_SYSTEM, L_INFO, 0,
            "postgres: loaded; libpq %d, jansson " JANSSON_VERSION,
            PQlibVersion());

  return 0;
}

/** Stop the pools and stand down.
 * @param[in] mod Handle for this module.
 */
static void pg_fini(struct ModuleHandle* mod)
{
  /* The server has already stopped this module's worker threads by the
   * time mi_fini runs -- it does that before the code can be unmapped --
   * so this is about the queues: whatever is still on one is answered with
   * DB_ERR_UNAVAILABLE rather than left waiting for a thread that is gone.
   */
  pg_pools_stop();

  db_unregister_driver(mod);

  pg_module = 0;
}

/** Notice a changed @c Database{} block.
 * @param[in] mod Handle for this module.
 */
static void pg_rehash(struct ModuleHandle* mod)
{
  (void) mod;

  /* Cheap when nothing moved, and when something did it stops the pools so
   * the next query builds them against the new configuration.  Doing it
   * here rather than at the next query means an operator who fixes a DSN
   * sees the connections come back without having to provoke one.
   */
  pg_pools_sync();
  pg_pools_report();
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "postgres",
  "1.0.0",
  "ircu developers",
  "PostgreSQL driver for the database API in db.h",
  pg_init,
  pg_fini,
  pg_rehash
};
