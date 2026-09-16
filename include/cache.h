/*
 * IRC - Internet Relay Chat, include/cache.h
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
 * @brief A key-value cache in front of the database.
 *
 * The same shape as db.h, and for the same two reasons: modules are
 * @c RTLD_LOCAL and cannot resolve each other's symbols, so the core has
 * to be the meeting point between whoever caches and whoever implements
 * the cache; and holding the calls in flight here is what lets the driver
 * module be unloaded with some outstanding.  The core knows nothing about
 * Redis: it knows there is a store with keys, values and an expiry, and
 * that it may not be there.  @c modules/workers/redis/ is the driver.
 *
 * **The cache is never the truth.**  Every caller reads it first and the
 * database second, and a cache that is missing, empty, stale or broken
 * only ever costs a query.  That is why a missing driver is not an error
 * anybody has to handle specially: cache_get() returns zero, the callback
 * never runs, and the caller goes to the database exactly as it would on
 * a miss.  The failure that matters is the database not answering, and
 * that is db.h's to report.
 *
 * Values are opaque bytes to the core.  What actually goes in them is
 * JSON, because the things worth caching have more than one field and a
 * string with separators in it is a format that will need versioning;
 * jansson is already in the tree for the database driver.
 *
 * **Invalidate on write, do not wait for the expiry.**  A TTL is the net
 * under whatever is written outside the server, not the mechanism: the
 * store is shared, so deleting a key on one server deletes it for all of
 * them, and the alternative is every other server serving a stale answer
 * until the clock runs out.
 */
#ifndef INCLUDED_cache_h
#define INCLUDED_cache_h

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

struct ModuleHandle;

/** Handle for one call in flight.  Zero is never a valid one. */
typedef unsigned long cache_id_t;

/** Longest key the core will carry, prefix included. */
#define CACHE_KEY_MAX 255
/** Longest value.  Bigger than this belongs in the database, not here. */
#define CACHE_VALUE_MAX 65536
/** Default expiry, in seconds, when a caller asks for none. */
#define CACHE_TTL_DEFAULT 300
/** Ceiling on the expiry.  A cache entry that outlives the server that
 * wrote it is one nobody remembers the reason for. */
#define CACHE_TTL_MAX 86400
/** Default milliseconds a call may take. */
#define CACHE_TIMEOUT_DEFAULT_MS 200
/** Ceiling on that.  A cache slower than this is not a cache: the point
 * of asking it first is that it answers before the database would. */
#define CACHE_TIMEOUT_MAX_MS 2000

/** What went wrong, or #CACHE_OK. */
enum CacheError {
  CACHE_OK,                /**< The call worked.  A miss is still OK. */
  CACHE_ERR_UNAVAILABLE,   /**< No driver, or it went away mid-call. */
  CACHE_ERR_CONFIG,        /**< The Redis{} block is missing or unusable. */
  CACHE_ERR_CONNECT,       /**< Could not reach the store. */
  CACHE_ERR_TIMEOUT,       /**< It did not answer in time. */
  CACHE_ERR_TOO_BIG,       /**< Key or value past the limits above. */
  CACHE_ERR_BACKEND,       /**< The store refused, or the driver failed. */
  CACHE_ERR_LAST           /**< Number of errors. */
};

/** The answer to one call. */
struct CacheResult {
  enum CacheError cres_code;   /**< #CACHE_OK, or what went wrong. */
  const char*     cres_key;    /**< Key that was asked about. */
  const char*     cres_value;  /**< Value, or NULL on a miss or an error. */
  size_t          cres_len;    /**< Its length. */
  int             cres_hit;    /**< Non-zero when the key was there. */
  const char*     cres_message;/**< Detail, or NULL for cache_strerror(). */
};

/** Called in the main thread when a call is answered.
 * @param[in] res The answer.  Nothing in it outlives the call.
 * @param[in] user Opaque pointer the caller passed in.
 */
typedef void (*CacheResultFn)(const struct CacheResult* res, void* user);

/** What a cache driver must do.  A module registers one of these. */
struct CacheDriver {
  /** Short name, for logs and /STATS. */
  const char* cdrv_name;

  /** Read a key.  Answers later with cache_complete().
   * @param[in] id Handle to hand back.
   * @param[in] key Key to read, prefix already applied.
   * @return #CACHE_OK when the call was queued.
   */
  enum CacheError (*cdrv_get)(cache_id_t id, const char* key);

  /** Write a key with an expiry.
   * @param[in] id Handle to hand back.
   * @param[in] key Key to write, prefix already applied.
   * @param[in] value Bytes to store; copied by the driver.
   * @param[in] len How many.
   * @param[in] ttl Seconds until it expires.
   * @return #CACHE_OK when the call was queued.
   */
  enum CacheError (*cdrv_set)(cache_id_t id, const char* key,
                              const char* value, size_t len, int ttl);

  /** Delete a key.
   * @param[in] id Handle to hand back.
   * @param[in] key Key to delete, prefix already applied.
   * @return #CACHE_OK when the call was queued.
   */
  enum CacheError (*cdrv_del)(cache_id_t id, const char* key);

  /** Forget a call.  The core has stopped caring about the answer. */
  void (*cdrv_cancel)(cache_id_t id);
};

/*
 * The register.
 */

/** Register the loaded module as the cache driver.
 *
 * One at a time, like the database driver: a second registration is
 * refused rather than replacing the first.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] driver Static description; must outlive the module.
 * @return Non-zero on success.
 */
extern int cache_register_driver(struct ModuleHandle* mod,
                                 const struct CacheDriver* driver);

/** Withdraw the driver, failing every call in flight first.
 * @param[in] mod Handle that registered it.
 */
extern void cache_unregister_driver(struct ModuleHandle* mod);

/** Non-zero if a driver is registered and configured. */
extern int cache_available(void);

/** Name of the registered driver, or "none". */
extern const char* cache_driver_name(void);

/*
 * Using it.
 */

/** Read a key.
 *
 * @param[in] mod Module asking, or NULL for the core.  Its calls are
 *   failed if it is unloaded before they are answered.
 * @param[in] key Key, without the configured prefix.
 * @param[in] fn Called with the answer, or NULL to fire and forget.
 * @param[in] user Opaque pointer for \a fn.
 * @return The handle, or 0 when there is no cache -- in which case \a fn
 *   is not called and the caller simply goes to the database.
 */
extern cache_id_t cache_get(struct ModuleHandle* mod, const char* key,
                            CacheResultFn fn, void* user);

/** Write a key.
 * @param[in] mod Module asking, or NULL for the core.
 * @param[in] key Key, without the configured prefix.
 * @param[in] value Bytes to store.
 * @param[in] len How many, or 0 to use strlen().
 * @param[in] ttl Seconds until it expires, or 0 for #CACHE_TTL_DEFAULT.
 * @param[in] fn Called with the answer, or NULL.
 * @param[in] user Opaque pointer for \a fn.
 * @return The handle, or 0.
 */
extern cache_id_t cache_set(struct ModuleHandle* mod, const char* key,
                            const char* value, size_t len, int ttl,
                            CacheResultFn fn, void* user);

/** Delete a key.
 *
 * What every writer calls after it has written to the database.  See the
 * note about invalidation at the top of this file.
 * @param[in] mod Module asking, or NULL for the core.
 * @param[in] key Key, without the configured prefix.
 * @param[in] fn Called with the answer, or NULL.
 * @param[in] user Opaque pointer for \a fn.
 * @return The handle, or 0.
 */
extern cache_id_t cache_del(struct ModuleHandle* mod, const char* key,
                            CacheResultFn fn, void* user);

/** Deliver the answer to call \a id.  Main thread only.
 *
 * Consumes the handle: the driver calls this exactly once for every call
 * it accepted.  A handle the core no longer holds is not an error.
 *
 * @param[in] id Handle the driver was given.
 * @param[in] value Bytes read, or NULL for a miss or a failure.
 * @param[in] len Their length.
 * @param[in] code #CACHE_OK, or what went wrong.
 * @param[in] message Detail, or NULL to use cache_strerror().
 * @return Non-zero if the handle was outstanding.
 */
extern int cache_complete(cache_id_t id, const char* value, size_t len,
                          enum CacheError code, const char* message);

/** Fail every call whose deadline has passed.
 * @param[in] now Current time.
 * @return How many expired.
 */
extern int cache_expire(time_t now);

/** Calls currently in flight. */
extern unsigned int cache_calls_pending(void);
/** Calls made since start-up. */
extern unsigned int cache_calls_total(void);
/** Of those, how many failed -- not counting misses, which are not
 * failures. */
extern unsigned int cache_calls_failed(void);
/** Of those, how many found the key. */
extern unsigned int cache_calls_hit(void);

/** Text for an error, for a log line. */
extern const char* cache_strerror(enum CacheError code);

/*
 * Configuration: the Redis{} block.
 *
 * The block is the core's because the connection is the server's, not a
 * module's -- the same reasoning that puts Database{} here.  The core does
 * not use a single field of it; it keeps it so that a driver loaded before
 * or after the block, or not at all, can ask.
 */
struct CacheConf {
  char* cconf_host;            /**< Host to connect to. */
  int   cconf_port;            /**< Port. */
  char* cconf_password;        /**< AUTH password, or NULL. */
  char* cconf_socket;          /**< Unix socket, instead of host and port. */
  int   cconf_database;        /**< Which numbered database. */
  int   cconf_pool;            /**< Connections to keep. */
  int   cconf_timeout_ms;      /**< Per-call deadline, clamped. */
  char* cconf_prefix;          /**< Prepended to every key. */
  unsigned int cconf_generation; /**< Bumped every time this changes. */
};

/** The configuration, or NULL if ircd.conf has no @c Redis{} block. */
extern const struct CacheConf* cache_conf(void);

/*
 * Server-side interface.  Not for use by modules.
 */

/** Reset the configuration; the parser calls this for each @c Redis{}. */
extern void cache_conf_clear(void);
/** Set the host. */
extern void cache_conf_set_host(char* host);
/** Set the port. */
extern void cache_conf_set_port(int port);
/** Set the AUTH password. */
extern void cache_conf_set_password(char* password);
/** Set a Unix socket path, used instead of host and port. */
extern void cache_conf_set_socket(char* path);
/** Set the numbered database. */
extern void cache_conf_set_database(int n);
/** Set how many connections the driver should keep. */
extern void cache_conf_set_pool(int size);
/** Set the per-call deadline, in milliseconds; clamped on commit. */
extern void cache_conf_set_timeout(int ms);
/** Set the key prefix. */
extern void cache_conf_set_prefix(char* prefix);
/** Finish a @c Redis{} block: validate it and publish it.
 * @param[out] errstr Receives a reason when this returns zero.
 * @return Non-zero when the block was accepted.
 */
extern int cache_conf_commit(const char** errstr);

/** Forget that a @c Redis{} block was seen; before reading the file. */
extern void cache_conf_unmark(void);
/** Drop the configuration if this pass had no @c Redis{} block. */
extern void cache_conf_sweep(void);

/** Drop a module's driver and its pending calls; module.c only. */
extern void cache_drop_module(struct ModuleHandle* mod);

/** Release everything; main() only, at exit. */
extern void cache_shutdown(void);

#endif /* INCLUDED_cache_h */
