/*
 * IRC - Internet Relay Chat, ircd/cache.c
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
 * @brief The cache driver register, the calls in flight and the Redis{} block.
 */
#include "config.h"

#include "cache.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_events.h"
#include "ircd_log.h"
#include "ircd_string.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** One call the driver has not answered yet. */
struct CacheCall {
  struct CacheCall*    cc_next;     /**< Next call, in no order. */
  cache_id_t           cc_id;       /**< What the driver holds. */
  CacheResultFn        cc_fn;       /**< Callback, or NULL. */
  void*                cc_user;     /**< The caller's opaque pointer. */
  struct ModuleHandle* cc_owner;    /**< Module that asked, or NULL. */
  time_t               cc_deadline; /**< When it is given up on. */
  char                 cc_key[CACHE_KEY_MAX + 1]; /**< Key, prefix included. */
};

/** The registered driver, or NULL. */
static const struct CacheDriver* cache_driver;
/** Module that registered it. */
static struct ModuleHandle* cache_driver_owner;

/** Calls accepted and not yet answered. */
static struct CacheCall* cache_calls;
/** Handle for the next call; never zero, never reused while in flight. */
static cache_id_t cache_last_id;

/** The published @c Redis{} block, or NULL. */
static struct CacheConf* cache_config;
/** The block being read, between cache_conf_clear() and commit. */
static struct CacheConf cache_pending;
/** Non-zero once cache_conf_clear() has run in this configuration pass. */
static int cache_pending_seen;

/** Statistics for cache_calls_pending() and friends. */
static unsigned int cache_stat_pending;
static unsigned int cache_stat_total;   /**< @copydoc cache_stat_pending */
static unsigned int cache_stat_failed;  /**< @copydoc cache_stat_pending */
static unsigned int cache_stat_hit;     /**< @copydoc cache_stat_pending */

/** Ceiling on calls in flight, so a driver that accepts and never answers
 * cannot grow the list without bound. */
#define CACHE_PENDING_MAX 8192

/** Text for each #CacheError, indexed by the enum. */
static const char* cache_error_text[CACHE_ERR_LAST] = {
  "no error",
  "no cache driver is loaded",
  "the Redis{} block is missing or unusable",
  "could not connect to the cache",
  "the cache did not answer in time",
  "the key or value is too large",
  "the cache driver failed"
};

/** Text for an error, for a log line.
 * @param[in] code What went wrong.
 */
const char* cache_strerror(enum CacheError code)
{
  if (code < 0 || code >= CACHE_ERR_LAST)
    return "unknown cache error";

  return cache_error_text[code];
}

/* ------------------------------------------------------------------- *
 * Calls in flight.                                                    *
 * ------------------------------------------------------------------- */

/** Timer over the earliest deadline, armed only while something is asked.
 *
 * Its own timer for the reason hooks.c and account.c have one: check_pings()
 * is scheduled minutes ahead on an idle server, and a deadline enforced
 * eventually is not a deadline.  Nothing is armed while nothing is in
 * flight, so a server with no cache module pays for none of it.
 */
static struct Timer cache_timer;

/** Whether #cache_timer is on the queue. */
static int cache_timer_armed;

static void cache_timeout(struct Event* ev);

/** Find a call by handle. */
static struct CacheCall* cache_find(cache_id_t id)
{
  struct CacheCall* call;

  for (call = cache_calls; call; call = call->cc_next)
    if (call->cc_id == id)
      return call;

  return NULL;
}

/** Earliest deadline of anything in flight, or 0. */
static time_t cache_deadline(void)
{
  struct CacheCall* call;
  time_t earliest = 0;

  for (call = cache_calls; call; call = call->cc_next)
    if (!earliest || call->cc_deadline < earliest)
      earliest = call->cc_deadline;

  return earliest;
}

/** Make sure the timer will fire by the earliest deadline. */
static void cache_arm(void)
{
  time_t deadline;

  if (cache_timer_armed)
    return;

  if (!(deadline = cache_deadline()))
    return;

  timer_add(timer_init(&cache_timer), cache_timeout, 0, TT_ABSOLUTE,
            deadline);
  cache_timer_armed = 1;
}

/** Fail whatever has waited too long, and set the timer for the rest. */
static void cache_timeout(struct Event* ev)
{
  if (ev_type(ev) != ET_EXPIRE) {
    if (ev_type(ev) == ET_DESTROY)
      cache_timer_armed = 0;
    return;
  }

  cache_timer_armed = 0;

  cache_expire(CurrentTime);
  cache_arm();
}

/** Hand out a handle that is neither zero nor already in flight. */
static cache_id_t cache_new_id(void)
{
  do {
    ++cache_last_id;
  } while (cache_last_id == 0 || cache_find(cache_last_id));

  return cache_last_id;
}

/** Unlink and free one call.  Does not call anything back. */
static void cache_free(struct CacheCall* call)
{
  struct CacheCall** call_p;

  for (call_p = &cache_calls; *call_p; call_p = &(*call_p)->cc_next) {
    if (*call_p == call) {
      *call_p = call->cc_next;
      if (cache_stat_pending)
        cache_stat_pending--;
      break;
    }
  }

  MyFree(call);
}

/** Take a call off the list and answer it.
 *
 * Off the list before the callback runs: a callback that asks something
 * else of the cache must not find this entry still linked.
 */
static void cache_answer(struct CacheCall* call, const char* value,
                         size_t len, enum CacheError code,
                         const char* message)
{
  struct CacheResult res;
  CacheResultFn fn = call->cc_fn;
  void* user = call->cc_user;
  char key[CACHE_KEY_MAX + 1];

  ircd_strncpy(key, call->cc_key, CACHE_KEY_MAX);

  cache_free(call);

  if (code != CACHE_OK)
    cache_stat_failed++;
  else if (value)
    cache_stat_hit++;

  if (!fn)
    return;

  memset(&res, 0, sizeof(res));
  res.cres_code = code;
  res.cres_key = key;
  res.cres_value = value;
  res.cres_len = len;
  res.cres_hit = (code == CACHE_OK && value != NULL);
  res.cres_message = message;

  (*fn)(&res, user);
}

/* ------------------------------------------------------------------- *
 * The register.                                                       *
 * ------------------------------------------------------------------- */

/** Register the loaded module as the cache driver.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] driver Static description of the driver.
 * @return Non-zero on success.
 */
int cache_register_driver(struct ModuleHandle* mod,
                          const struct CacheDriver* driver)
{
  if (!driver || !driver->cdrv_name || !driver->cdrv_get
      || !driver->cdrv_set || !driver->cdrv_del) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Refusing a cache driver that is missing a name, get, set "
              "or del");
    return 0;
  }

  if (cache_driver) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Refusing cache driver %s: %s is already registered",
              driver->cdrv_name, cache_driver->cdrv_name);
    return 0;
  }

  cache_driver = driver;
  cache_driver_owner = mod;

  log_write(LS_SYSTEM, L_INFO, 0, "Cache driver %s registered",
            driver->cdrv_name);

  return 1;
}

/** Withdraw the driver, failing every call in flight first.
 * @param[in] mod Handle that registered it.
 */
void cache_unregister_driver(struct ModuleHandle* mod)
{
  const char* name;

  if (!cache_driver || cache_driver_owner != mod)
    return;

  name = cache_driver->cdrv_name;

  /* The register goes first, so a callback that asks for something on its
   * way out is refused rather than reaching code being unmapped.
   */
  cache_driver = NULL;
  cache_driver_owner = NULL;

  while (cache_calls)
    cache_answer(cache_calls, NULL, 0, CACHE_ERR_UNAVAILABLE, NULL);

  log_write(LS_SYSTEM, L_INFO, 0, "Cache driver %s withdrawn", name);
}

/** Non-zero if a driver is registered and configured. */
int cache_available(void)
{
  return cache_driver != NULL && cache_config != NULL;
}

/** Name of the registered driver, or "none". */
const char* cache_driver_name(void)
{
  return cache_driver ? cache_driver->cdrv_name : "none";
}

/** Drop a module's driver and its pending calls.
 * @param[in] mod Module being torn down.
 */
void cache_drop_module(struct ModuleHandle* mod)
{
  struct CacheCall* call;
  struct CacheCall* next;

  if (!mod)
    return;

  cache_unregister_driver(mod);

  /* A module may have asked for things without being the driver. */
  for (call = cache_calls; call; call = next) {
    next = call->cc_next;

    if (call->cc_owner != mod)
      continue;

    if (cache_driver && cache_driver->cdrv_cancel)
      (*cache_driver->cdrv_cancel)(call->cc_id);

    cache_free(call);
  }
}

/* ------------------------------------------------------------------- *
 * Using it.                                                           *
 * ------------------------------------------------------------------- */

/** Start a call, with the key prefixed and the deadline set.
 * @return The new entry, or NULL if the cache cannot be asked.
 */
static struct CacheCall* cache_begin(struct ModuleHandle* mod,
                                     const char* key, CacheResultFn fn,
                                     void* user)
{
  struct CacheCall* call;
  const char* prefix;

  if (!cache_available() || EmptyString(key))
    return NULL;

  if (cache_stat_pending >= CACHE_PENDING_MAX) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Cache driver %s has %u calls outstanding; refusing another",
              cache_driver->cdrv_name, cache_stat_pending);
    return NULL;
  }

  call = (struct CacheCall*) MyCalloc(1, sizeof(struct CacheCall));

  /* The prefix is applied here and nowhere else: a driver that had to
   * remember to do it would be a driver that one day forgot, and two
   * networks sharing a store would read each other's keys.
   */
  prefix = cache_config->cconf_prefix ? cache_config->cconf_prefix : "";
  if (strlen(prefix) + strlen(key) > CACHE_KEY_MAX) {
    MyFree(call);
    return NULL;
  }
  strcpy(call->cc_key, prefix);
  strcat(call->cc_key, key);

  call->cc_id = cache_new_id();
  call->cc_fn = fn;
  call->cc_user = user;
  call->cc_owner = mod;
  call->cc_deadline = CurrentTime
    + (cache_config->cconf_timeout_ms + 999) / 1000 + 1;

  call->cc_next = cache_calls;
  cache_calls = call;
  cache_stat_pending++;
  cache_stat_total++;

  cache_arm();

  return call;
}

/** Give up on a call the driver refused outright. */
static cache_id_t cache_refused(struct CacheCall* call, enum CacheError code)
{
  cache_answer(call, NULL, 0, code, NULL);
  return 0;
}

/** Read a key.
 * @param[in] mod Module asking, or NULL for the core.
 * @param[in] key Key, without the configured prefix.
 * @param[in] fn Called with the answer, or NULL.
 * @param[in] user Opaque pointer for \a fn.
 * @return The handle, or 0 when there is no cache.
 */
cache_id_t cache_get(struct ModuleHandle* mod, const char* key,
                     CacheResultFn fn, void* user)
{
  struct CacheCall* call;
  enum CacheError code;
  cache_id_t id;

  if (!(call = cache_begin(mod, key, fn, user)))
    return 0;

  id = call->cc_id;

  if ((code = (*cache_driver->cdrv_get)(id, call->cc_key)) != CACHE_OK)
    return cache_refused(call, code);

  return id;
}

/** Write a key.
 * @param[in] mod Module asking, or NULL for the core.
 * @param[in] key Key, without the configured prefix.
 * @param[in] value Bytes to store.
 * @param[in] len How many, or 0 for strlen().
 * @param[in] ttl Seconds until it expires, or 0 for the default.
 * @param[in] fn Called with the answer, or NULL.
 * @param[in] user Opaque pointer for \a fn.
 * @return The handle, or 0.
 */
cache_id_t cache_set(struct ModuleHandle* mod, const char* key,
                     const char* value, size_t len, int ttl,
                     CacheResultFn fn, void* user)
{
  struct CacheCall* call;
  enum CacheError code;
  cache_id_t id;

  if (!value)
    return 0;
  if (!len)
    len = strlen(value);

  if (len > CACHE_VALUE_MAX) {
    log_write(LS_SYSTEM, L_WARNING, 0,
              "Refusing to cache %s: %lu bytes is past the %d limit",
              key, (unsigned long) len, CACHE_VALUE_MAX);
    return 0;
  }

  if (ttl <= 0)
    ttl = CACHE_TTL_DEFAULT;
  if (ttl > CACHE_TTL_MAX)
    ttl = CACHE_TTL_MAX;

  if (!(call = cache_begin(mod, key, fn, user)))
    return 0;

  id = call->cc_id;

  if ((code = (*cache_driver->cdrv_set)(id, call->cc_key, value, len, ttl))
      != CACHE_OK)
    return cache_refused(call, code);

  return id;
}

/** Delete a key.
 * @param[in] mod Module asking, or NULL for the core.
 * @param[in] key Key, without the configured prefix.
 * @param[in] fn Called with the answer, or NULL.
 * @param[in] user Opaque pointer for \a fn.
 * @return The handle, or 0.
 */
cache_id_t cache_del(struct ModuleHandle* mod, const char* key,
                     CacheResultFn fn, void* user)
{
  struct CacheCall* call;
  enum CacheError code;
  cache_id_t id;

  if (!(call = cache_begin(mod, key, fn, user)))
    return 0;

  id = call->cc_id;

  if ((code = (*cache_driver->cdrv_del)(id, call->cc_key)) != CACHE_OK)
    return cache_refused(call, code);

  return id;
}

/** Deliver the answer to call \a id.
 * @param[in] id Handle the driver was given.
 * @param[in] value Bytes read, or NULL for a miss or a failure.
 * @param[in] len Their length.
 * @param[in] code #CACHE_OK, or what went wrong.
 * @param[in] message Detail, or NULL.
 * @return Non-zero if the handle was outstanding.
 */
int cache_complete(cache_id_t id, const char* value, size_t len,
                   enum CacheError code, const char* message)
{
  struct CacheCall* call = cache_find(id);

  if (!call) {
    log_write(LS_SYSTEM, L_INFO, 0,
              "cache_complete() for %lu, which is no longer outstanding",
              (unsigned long) id);
    return 0;
  }

  cache_answer(call, value, len, code, message);

  return 1;
}

/** Fail every call whose deadline has passed.
 *
 * One at a time, restarting the walk after each: a callback can ask for
 * something else, which puts a new entry on this list.
 *
 * @param[in] now Current time.
 * @return How many expired.
 */
int cache_expire(time_t now)
{
  struct CacheCall* call;
  int expired = 0;

  for (;;) {
    for (call = cache_calls; call; call = call->cc_next)
      if (call->cc_deadline <= now)
        break;

    if (!call)
      break;

    log_write(LS_SYSTEM, L_WARNING, 0,
              "Cache driver %s did not answer %s in time",
              cache_driver_name(), call->cc_key);

    if (cache_driver && cache_driver->cdrv_cancel)
      (*cache_driver->cdrv_cancel)(call->cc_id);

    cache_answer(call, NULL, 0, CACHE_ERR_TIMEOUT, NULL);
    expired++;
  }

  return expired;
}

/** Calls currently in flight. */
unsigned int cache_calls_pending(void)
{
  return cache_stat_pending;
}

/** Calls made since start-up. */
unsigned int cache_calls_total(void)
{
  return cache_stat_total;
}

/** Of those, how many failed. */
unsigned int cache_calls_failed(void)
{
  return cache_stat_failed;
}

/** Of those, how many found the key. */
unsigned int cache_calls_hit(void)
{
  return cache_stat_hit;
}

/* ------------------------------------------------------------------- *
 * The Redis{} block.                                                  *
 * ------------------------------------------------------------------- */

/** Release the strings a configuration holds. */
static void cache_conf_release(struct CacheConf* conf)
{
  MyFree(conf->cconf_host);
  MyFree(conf->cconf_password);
  MyFree(conf->cconf_socket);
  MyFree(conf->cconf_prefix);
  memset(conf, 0, sizeof(*conf));
}

/** The configuration, or NULL if there is no @c Redis{} block. */
const struct CacheConf* cache_conf(void)
{
  return cache_config;
}

void cache_conf_clear(void)
{
  cache_conf_release(&cache_pending);
  cache_pending.cconf_port = 6379;
  cache_pending.cconf_pool = 2;
  cache_pending.cconf_timeout_ms = CACHE_TIMEOUT_DEFAULT_MS;
  cache_pending_seen = 1;
}

void cache_conf_set_host(char* host)
{
  MyFree(cache_pending.cconf_host);
  cache_pending.cconf_host = host;
}

void cache_conf_set_port(int port)
{
  cache_pending.cconf_port = port;
}

void cache_conf_set_password(char* password)
{
  MyFree(cache_pending.cconf_password);
  cache_pending.cconf_password = password;
}

void cache_conf_set_socket(char* path)
{
  MyFree(cache_pending.cconf_socket);
  cache_pending.cconf_socket = path;
}

void cache_conf_set_database(int n)
{
  cache_pending.cconf_database = n;
}

void cache_conf_set_pool(int size)
{
  cache_pending.cconf_pool = size;
}

void cache_conf_set_timeout(int ms)
{
  cache_pending.cconf_timeout_ms = ms;
}

void cache_conf_set_prefix(char* prefix)
{
  MyFree(cache_pending.cconf_prefix);
  cache_pending.cconf_prefix = prefix;
}

/** Finish a @c Redis{} block: validate it and publish it.
 * @param[out] errstr Receives a reason when this returns zero.
 * @return Non-zero when the block was accepted.
 */
int cache_conf_commit(const char** errstr)
{
  static unsigned int generation;

  assert(0 != errstr);

  /* Somewhere to connect: a host, or a socket instead of one. */
  if (EmptyString(cache_pending.cconf_host)
      && EmptyString(cache_pending.cconf_socket)) {
    *errstr = "Redis: host or socket is required";
    cache_conf_release(&cache_pending);
    return 0;
  }

  if (cache_pending.cconf_port <= 0 || cache_pending.cconf_port > 65535)
    cache_pending.cconf_port = 6379;

  if (cache_pending.cconf_pool <= 0)
    cache_pending.cconf_pool = 1;

  /* A cache that takes longer than this to answer is not a cache: asking
   * it first only pays if it answers before the database would.  Asking
   * for more is corrected rather than refused, so a server does not fail
   * to start over it, but it is said out loud.
   */
  if (cache_pending.cconf_timeout_ms <= 0)
    cache_pending.cconf_timeout_ms = CACHE_TIMEOUT_DEFAULT_MS;
  if (cache_pending.cconf_timeout_ms > CACHE_TIMEOUT_MAX_MS) {
    log_write(LS_CONFIG, L_WARNING, 0,
              "Redis: timeout of %dms exceeds the %dms maximum; using %dms",
              cache_pending.cconf_timeout_ms, CACHE_TIMEOUT_MAX_MS,
              CACHE_TIMEOUT_MAX_MS);
    cache_pending.cconf_timeout_ms = CACHE_TIMEOUT_MAX_MS;
  }

  if (!cache_config)
    cache_config = (struct CacheConf*) MyCalloc(1, sizeof(struct CacheConf));
  else
    cache_conf_release(cache_config);

  *cache_config = cache_pending;
  memset(&cache_pending, 0, sizeof(cache_pending));

  cache_config->cconf_generation = ++generation;

  return 1;
}

void cache_conf_unmark(void)
{
  cache_pending_seen = 0;
}

void cache_conf_sweep(void)
{
  /* The scratch block is finished with either way: a commit empties it,
   * and a block that never got as far as one leaves it holding strings
   * nobody will read.
   */
  cache_conf_release(&cache_pending);

  if (cache_pending_seen || !cache_config)
    return;

  /* The block was taken out of ircd.conf.  The driver keeps running
   * against what it connected to -- pulling its connections out from
   * under it on a rehash would fail every call in flight -- but nothing
   * new is asked, because cache_available() is false without a block.
   */
  log_write(LS_CONFIG, L_INFO, 0,
            "The Redis{} block is gone; the cache is no longer used");

  cache_conf_release(cache_config);
  MyFree(cache_config);
  cache_config = NULL;
}

/** Release everything; main() only, at exit. */
void cache_shutdown(void)
{
  while (cache_calls)
    cache_answer(cache_calls, NULL, 0, CACHE_ERR_UNAVAILABLE, NULL);

  if (cache_timer_armed) {
    timer_del(&cache_timer);
    cache_timer_armed = 0;
  }

  cache_conf_release(&cache_pending);

  if (cache_config) {
    cache_conf_release(cache_config);
    MyFree(cache_config);
    cache_config = NULL;
  }

  cache_driver = NULL;
  cache_driver_owner = NULL;

  /* Counters included: this is "release everything", and a reset that
   * leaves some state behind is one the tests reach for as a reset and
   * then have to work around.
   */
  cache_stat_pending = 0;
  cache_stat_total = 0;
  cache_stat_failed = 0;
  cache_stat_hit = 0;
}
