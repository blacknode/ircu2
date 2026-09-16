/*
 * IRC - Internet Relay Chat, modules/workers/redis/redis.c
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
 * @brief The Redis cache driver: what the server sees of it.
 *
 * The implementation behind cache.h.  It registers itself as the cache
 * driver, and from then on every cache_get(), cache_set() and cache_del()
 * in the server -- from any module, without any of them linking against
 * hiredis or knowing that it exists -- lands here, runs on a pooled
 * connection in a worker thread, and comes back as bytes.
 *
 * @code
 *   Redis {
 *     host = "127.0.0.1";
 *     port = 6379;
 *     password = "${REDIS_PASSWORD}";
 *     pool = 2;
 *     timeout_ms = 200;
 *     prefix = "ircu:";
 *   };
 *
 *   Module { name = "redis"; };
 * @endcode
 *
 * and set @c WORKER_THREADS to at least the size of the pool, because a
 * connection is a thread.  With workers off the module loads and refuses
 * every call, which costs the server a database query and nothing else --
 * the cache is never the truth.
 *
 * hiredis is used synchronously on purpose.  An event-driven client would
 * have to be woven into the server's own event loop, and the thing this
 * module exists to avoid -- the main thread waiting on a socket -- is
 * already solved by the worker threads the tree has.
 */
#include "config.h"

#include "redis.h"

#include "client.h"
#include "ircd_log.h"
#include "ircd_string.h"
#include "numeric.h"
#include "send.h"

#include <string.h>

/** This module's handle. */
static struct ModuleHandle* redis_mod;

/** The queue the connection threads take from. */
static struct RedisPool redis_pool;

/** The connection threads. */
#define REDIS_MAX_POOL 16
static struct Worker* redis_workers[REDIS_MAX_POOL];
static int redis_worker_count;

/** The configuration the threads were started against. */
static unsigned int redis_generation;

/** Non-zero once the pool has been set up. */
static int redis_started;

/** Stop every connection thread and empty the queue. */
static void redis_stop_pool(void)
{
  int i;

  if (!redis_started)
    return;

  redis_pool_stop(&redis_pool);

  for (i = 0; i < redis_worker_count; i++) {
    if (redis_workers[i])
      module_stop_worker(redis_mod, redis_workers[i]);
    redis_workers[i] = 0;
  }

  redis_worker_count = 0;

  /* Anything still queued is answered by the core's own deadline; what is
   * dropped here is the request, not the caller's expectation. */
  redis_pool_destroy(&redis_pool);
  redis_started = 0;
}

/** Start the connection threads against the current Redis{} block.
 * @return Non-zero when at least one thread is running.
 */
static int redis_start_pool(void)
{
  const struct CacheConf* conf = cache_conf();
  int wanted;
  int i;

  if (!conf)
    return 0;

  if (redis_started && redis_generation == conf->cconf_generation)
    return redis_worker_count > 0;

  redis_stop_pool();

  redis_pool_init(&redis_pool);
  redis_started = 1;
  redis_generation = conf->cconf_generation;

  ircd_strncpy(redis_pool.rp_host, conf->cconf_host ? conf->cconf_host : "",
               sizeof(redis_pool.rp_host) - 1);
  ircd_strncpy(redis_pool.rp_socket,
               conf->cconf_socket ? conf->cconf_socket : "",
               sizeof(redis_pool.rp_socket) - 1);
  ircd_strncpy(redis_pool.rp_password,
               conf->cconf_password ? conf->cconf_password : "",
               sizeof(redis_pool.rp_password) - 1);
  redis_pool.rp_port = conf->cconf_port;
  redis_pool.rp_database = conf->cconf_database;
  redis_pool.rp_timeout_ms = conf->cconf_timeout_ms;

  wanted = conf->cconf_pool;
  if (wanted > REDIS_MAX_POOL)
    wanted = REDIS_MAX_POOL;

  for (i = 0; i < wanted; i++) {
    redis_workers[i] = module_spawn_worker(redis_mod, "redis",
                                           redis_conn_main, &redis_pool);
    if (!redis_workers[i])
      break;

    redis_worker_count++;
  }

  if (!redis_worker_count) {
    log_write(LS_SYSTEM, L_WARNING, 0,
              "redis: no connection threads; is WORKER_THREADS zero?");
    return 0;
  }

  log_write(LS_SYSTEM, L_INFO, 0,
            "redis: %d connection%s to %s", redis_worker_count,
            redis_worker_count == 1 ? "" : "s",
            redis_pool.rp_socket[0] ? redis_pool.rp_socket
                                    : redis_pool.rp_host);

  return 1;
}

/** Build a request and put it on the queue.
 * @return #CACHE_OK when it was queued.
 */
static enum CacheError redis_submit(cache_id_t id, enum RedisOp op,
                                    const char* key, const char* value,
                                    size_t len, int ttl)
{
  struct RedisRequest* req;

  if (!redis_start_pool())
    return CACHE_ERR_CONNECT;

  if (!(req = (struct RedisRequest*) worker_alloc(sizeof(*req))))
    return CACHE_ERR_BACKEND;

  req->rr_id = id;
  req->rr_op = op;
  req->rr_ttl = ttl;

  if (!(req->rr_key = (char*) worker_alloc(strlen(key) + 1))) {
    redis_request_free(req);
    return CACHE_ERR_BACKEND;
  }
  strcpy(req->rr_key, key);

  if (value) {
    if (!(req->rr_value = (char*) worker_alloc(len + 1))) {
      redis_request_free(req);
      return CACHE_ERR_BACKEND;
    }
    memcpy(req->rr_value, value, len);
    req->rr_value[len] = '\0';
    req->rr_len = len;
  }

  if (!redis_pool_push(&redis_pool, req)) {
    redis_request_free(req);
    return CACHE_ERR_BACKEND;
  }

  return CACHE_OK;
}

/** Deliver one answer to the core.  Main thread, from the worker drain.
 * @param[in] task The finished request.
 */
void redis_deliver(struct WorkTask* task)
{
  struct RedisRequest* req = (struct RedisRequest*) task->wt_in;

  if (!req)
    return;

  if (req->rr_code != CACHE_OK && req->rr_error[0])
    log_write(LS_SYSTEM, L_WARNING, 0, "redis: %s: %s", req->rr_key,
              req->rr_error);

  cache_complete(req->rr_id, req->rr_hit ? req->rr_reply : 0,
                 req->rr_replylen, req->rr_code,
                 req->rr_error[0] ? req->rr_error : 0);
}

/** Release a task's payload.  The server calls this after wt_done.
 * @param[in] task Task being released.
 */
void redis_task_free(struct WorkTask* task)
{
  redis_request_free((struct RedisRequest*) task->wt_in);
  task->wt_in = 0;
}

/* ------------------------------------------------------------------------
 * The driver.
 * ------------------------------------------------------------------------ */

static enum CacheError redis_driver_get(cache_id_t id, const char* key)
{
  return redis_submit(id, REDIS_OP_GET, key, 0, 0, 0);
}

static enum CacheError redis_driver_set(cache_id_t id, const char* key,
                                        const char* value, size_t len,
                                        int ttl)
{
  return redis_submit(id, REDIS_OP_SET, key, value, len, ttl);
}

static enum CacheError redis_driver_del(cache_id_t id, const char* key)
{
  return redis_submit(id, REDIS_OP_DEL, key, 0, 0, 0);
}

/** Forget a call.
 *
 * Nothing to do: a request already handed to a connection thread cannot
 * be recalled, and the answer it eventually posts is delivered to a
 * handle the core no longer holds, which cache_complete() shrugs off.
 */
static void redis_driver_cancel(cache_id_t id)
{
  (void) id;
}

static const struct CacheDriver redis_driver = {
  "redis", redis_driver_get, redis_driver_set, redis_driver_del,
  redis_driver_cancel
};

/** Set up on load.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int redis_init(struct ModuleHandle* mod)
{
  redis_mod = mod;

  if (!module_add_cache_driver(mod, &redis_driver))
    return -1;

  /* The threads are not started here.  mi_init runs mid-parse, so the
   * Redis{} block may not have been read yet; the first call starts them
   * against whatever the configuration turned out to be.
   */
  return 0;
}

/** Tear down on unload.
 * @param[in] mod Handle for this module.
 */
static void redis_fini(struct ModuleHandle* mod)
{
  module_del_cache_driver(mod);
  redis_stop_pool();
  redis_mod = 0;
}

/** Re-read the configuration.
 *
 * The threads are only restarted when the block actually changed: a
 * rehash that did not touch it must not drop connections and fail every
 * call in flight for nothing.
 *
 * @param[in] mod Handle for this module.
 */
static void redis_rehash(struct ModuleHandle* mod)
{
  const struct CacheConf* conf = cache_conf();

  (void) mod;

  if (!conf) {
    redis_stop_pool();
    return;
  }

  if (redis_started && redis_generation != conf->cconf_generation)
    redis_start_pool();
}

/** Module description. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "redis",
  "1.0",
  "ircu2",
  "Redis cache driver (hiredis)",
  redis_init,
  redis_fini,
  redis_rehash
};
