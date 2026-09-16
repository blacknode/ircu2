/*
 * IRC - Internet Relay Chat, modules/services/identity/ident_lookup.c
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
 * @brief "Whose nickname is this?" -- Redis first, then PostgreSQL.
 *
 * The hot path of the whole phase: this question is asked at every
 * registration and every nick change on the network, so without a cache it
 * would put the database in the critical path of connecting.
 *
 * @verbatim
 *   whose is "maria"?
 *     -> cache GET nick:maria
 *          hit  -> answer, done
 *          miss v
 *     -> SELECT ... WHERE nick_canon = 'maria'
 *          a row -> cache SETEX {...}   -> REGISTERED
 *          none  -> cache SETEX {free}  -> FREE
 *          error -> UNKNOWN, which is not FREE
 * @endverbatim
 *
 * The misses are cached because "not registered" is the majority answer,
 * and a cache of hits only would leave most of the traffic reaching the
 * database anyway.  The entries are invalidated on write rather than left
 * to the TTL, which is what makes one server's change visible on all of
 * them; the TTL is the safety net for whatever is written outside the
 * ircd, not the mechanism.
 */
#include "config.h"

#include "cache.h"
#include "db.h"
#include "identity.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"

#include <jansson.h>
#include <string.h>

/** How long a nickname's answer stays in the cache, in seconds.
 *
 * Zero means #CACHE_TTL_DEFAULT.  It is short on purpose: the entry is
 * deleted when the account changes, so the TTL only has to cover a change
 * made behind the server's back.
 */
#define IDENT_NICK_TTL 0

/** The statement behind a lookup. */
static const char ident_lookup_sql[] =
  "SELECT a.nick AS nick,"
  "       (a.suspended_at IS NOT NULL) AS suspended"
  "  FROM account a"
  " WHERE a.nick_canon = $1";

/** Write the cache key for \a canon into \a buf. */
static void ident_nick_key(const char* canon, char* buf, size_t len)
{
  ircd_snprintf(0, buf, len, "nick:%s", canon);
}

/** Answer a lookup and release the request.
 * @param[in] req The request.
 * @param[in] owner What was established.
 */
static void ident_lookup_answer(struct IdentRequest* req,
                                enum AccountOwner owner)
{
  account_id_t id = req->ir_id;
  int cancelled = req->ir_cancelled;

  ident_free(req);

  if (!cancelled)
    account_complete_owner(id, owner);
}

/** Store what was found, so the next server does not ask again.
 *
 * Fire and forget: whether the write lands changes nothing about the
 * answer, which has already been given.
 *
 * @param[in] canon Canonical nickname.
 * @param[in] registered Non-zero if somebody holds it.
 */
static void ident_lookup_cache(const char* canon, int registered)
{
  char key[CACHE_KEY_MAX + 1];
  char value[64];

  if (!cache_available())
    return;

  ident_nick_key(canon, key, sizeof(key));

  /* An object and not a bare word, because what is known about a nickname
   * will grow -- suspended, which identity owns it -- and a value that is
   * a plain string is a format that has to be versioned the first time it
   * gains a second field. */
  ircd_snprintf(0, value, sizeof(value), "{\"registered\":%s}",
                registered ? "true" : "false");

  cache_set(ident_mod, key, value, 0, IDENT_NICK_TTL, NULL, NULL);
}

/** Take the database's answer.
 * @param[in] res The rows.
 * @param[in] user The request's serial.
 */
static void ident_lookup_rows(const struct DbResult* res, void* user)
{
  struct IdentRequest* req = ident_find((unsigned long) (size_t) user);
  int registered;

  if (!req)
    return;

  if (res->err.dberr_code != DB_OK) {
    /* The source of truth did not answer.  That is not "nobody has it":
     * see proposal 007 section 7 and the note in identity.c. */
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: could not establish who holds %s: %s",
              req->ir_nick, res->err.dberr_message);
    ident_lookup_answer(req, ACCOUNT_NICK_UNKNOWN);
    return;
  }

  registered = ident_rows(res) > 0;

  ident_lookup_cache(req->ir_canon, registered);

  ident_lookup_answer(req, registered ? ACCOUNT_NICK_REGISTERED
                                      : ACCOUNT_NICK_FREE);
}

/** Ask the database.
 * @param[in] req The request.
 */
static void ident_lookup_db(struct IdentRequest* req)
{
  struct DbParam canon;
  struct DbParam* params[2];
  struct DbQuery query;
  enum DbError err;

  if (!ident_db_ready("a nickname lookup")) {
    ident_lookup_answer(req, ACCOUNT_NICK_UNKNOWN);
    return;
  }

  canon.type = DB_TYPE_TEXT;
  canon.value = req->ir_canon;
  canon.format = DB_FORMAT_TEXT;

  params[0] = &canon;
  params[1] = NULL;

  query.sql = ident_lookup_sql;
  query.params = params;

  err = db_query(ident_mod, &query, ident_lookup_rows,
                 (void*) (size_t) req->ir_serial);

  if (err != DB_OK) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: could not ask who holds %s: %s", req->ir_nick,
              db_strerror(err));
    ident_lookup_answer(req, ACCOUNT_NICK_UNKNOWN);
  }
}

/** Take the cache's answer.
 *
 * Only a hit is used.  A miss, a timeout, a store that is down and a
 * driver that was unloaded mid-call all mean the same thing here: ask the
 * database, which is where the truth was all along.
 *
 * @param[in] res The answer.
 * @param[in] user The request's serial.
 */
static void ident_lookup_cached(const struct CacheResult* res, void* user)
{
  struct IdentRequest* req = ident_find((unsigned long) (size_t) user);
  json_error_t error;
  json_t* parsed;
  json_t* registered;
  enum AccountOwner owner;

  if (!req)
    return;

  if (res->cres_code != CACHE_OK || !res->cres_hit || !res->cres_value) {
    ident_lookup_db(req);
    return;
  }

  parsed = json_loadb(res->cres_value, res->cres_len, 0, &error);

  if (!parsed || !json_is_object(parsed)
      || !(registered = json_object_get(parsed, "registered"))
      || !json_is_boolean(registered)) {
    /* Somebody else's key, or one this version does not understand.  The
     * database is right there. */
    if (parsed)
      json_decref(parsed);

    ident_lookup_db(req);
    return;
  }

  owner = json_is_true(registered) ? ACCOUNT_NICK_REGISTERED
                                   : ACCOUNT_NICK_FREE;
  json_decref(parsed);

  ident_lookup_answer(req, owner);
}

/** Find out whether a nickname is registered.
 * @param[in] id Handle to answer with.
 * @param[in] nick The nickname.
 */
void ident_lookup(account_id_t id, const char* nick)
{
  struct IdentRequest* req = ident_begin(id, IDENT_LOOKUP);
  char key[CACHE_KEY_MAX + 1];

  ircd_strncpy(req->ir_nick, nick ? nick : "", NICKLEN);
  ident_nick_canon(req->ir_nick, req->ir_canon, sizeof(req->ir_canon));

  if (!*req->ir_canon) {
    ident_lookup_answer(req, ACCOUNT_NICK_UNKNOWN);
    return;
  }

  ident_nick_key(req->ir_canon, key, sizeof(key));

  /* Zero is no cache at all, which is the same thing as a miss and needs
   * no branch of its own beyond going straight on to the database. */
  if (!cache_get(ident_mod, key, ident_lookup_cached,
                 (void*) (size_t) req->ir_serial))
    ident_lookup_db(req);
}
