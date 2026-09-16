/*
 * IRC - Internet Relay Chat, modules/services/identity/identity.c
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
 * @brief The identity provider: the store behind +r.
 *
 * This module is the mechanism and nothing else.  It has no bot, registers
 * no commands and never sends a line to a user: it registers the provider
 * in include/account.h and answers three questions -- is this credential
 * good, whose nickname is this, what accounts does this address hold --
 * and the core does everything visible.  The policy and the voice are
 * nickserv's (proposal 007 section 9).
 *
 * It owns the schema, which is why it is a directory module: its
 * migrations/ is compiled into the .so and an operator applies it with
 * /MODULE MIGRATION APPLY identity.
 *
 * Three rules shape everything here.
 *
 *   - @b The @b password @b never @b touches @b the @b main @b thread.
 *     Argon2 takes 50-250 ms and tens of megabytes on purpose, so it goes
 *     to a worker (see worker.h and ircd_pwhash.h, which is pure exactly
 *     so that it can).  That means a verification is two hops -- a query,
 *     then a worker -- and no wait anywhere.
 *
 *   - @b The @b cache @b is @b never @b the @b truth.  The nickname lookup
 *     reads Redis first and PostgreSQL after a miss, and a cache that is
 *     down, empty or stale costs a query and nothing else.  Misses are
 *     cached too: "not registered" is the majority answer, and a cache of
 *     hits only would leave most of the traffic reaching the database
 *     anyway.
 *
 *   - @b What @b cannot @b be @b established @b is @b not @b free.  A
 *     lookup that fails answers #ACCOUNT_NICK_UNKNOWN, never
 *     #ACCOUNT_NICK_FREE.  The one moment an impostor gets somebody else's
 *     nickname is the moment the service protecting it is not answering,
 *     and that moment can be arranged; see proposal 007 section 7.
 *
 * Writing -- registering an account, suspending one, changing a password --
 * is not here yet.  It arrives with nickserv, which is its only caller,
 * and brings the advisory locks of section 9.1 with it.
 */
#include "config.h"

#include "client.h"
#include "db.h"
#include "identity.h"
#include "ircd_alloc.h"
#include "ircd_chattr.h"
#include "ircd_env.h"
#include "ircd_log.h"
#include "ircd_sha256.h"   /* ircd_crypto_wipe() */
#include "ircd_string.h"
#include "module.h"
#include "worker.h"

#include <jansson.h>
#include <string.h>

/** This module's handle. */
struct ModuleHandle* ident_mod;

/** Questions accepted and not yet answered. */
static struct IdentRequest* ident_requests;

/** Handle for the next one; ours, not the core's. */
static unsigned long ident_last_serial;

/** The provider the core gets. */
static const struct AccountProvider ident_provider = {
  "identity",
  ident_verify,
  ident_lookup,
  ident_list,
  ident_cancel
};

/* ------------------------------------------------------------------- *
 * Requests                                                            *
 * ------------------------------------------------------------------- */

/** Start tracking a question.
 * @param[in] id What the core will expect back.
 * @param[in] kind Which question.
 * @return The request, or NULL if there is no memory.
 */
struct IdentRequest* ident_begin(account_id_t id, enum IdentKind kind)
{
  struct IdentRequest* req;

  req = (struct IdentRequest*) MyCalloc(1, sizeof(*req));
  req->ir_serial = ++ident_last_serial;
  req->ir_id = id;
  req->ir_kind = kind;

  req->ir_next = ident_requests;
  ident_requests = req;

  return req;
}

/** Find a request by the handle this module gave it.
 *
 * Callbacks carry the serial rather than the pointer: a pointer would be
 * right until the day an answer arrives for a request that is already
 * gone, and then it would be a use-after-free instead of a NULL.
 *
 * @param[in] serial Handle from IdentRequest::ir_serial.
 * @return The request, or NULL.
 */
struct IdentRequest* ident_find(unsigned long serial)
{
  struct IdentRequest* req;

  for (req = ident_requests; req; req = req->ir_next)
    if (req->ir_serial == serial)
      return req;

  return NULL;
}

/** Unlink and release a request.  Answers nothing.
 * @param[in] req Request to release.
 */
void ident_free(struct IdentRequest* req)
{
  struct IdentRequest** p;

  if (!req)
    return;

  for (p = &ident_requests; *p; p = &(*p)->ir_next) {
    if (*p == req) {
      *p = req->ir_next;
      break;
    }
  }

  /* The password, if it never got as far as a worker. */
  if (req->ir_work) {
    ircd_crypto_wipe(req->ir_work, sizeof(*req->ir_work));
    worker_free(req->ir_work);
    req->ir_work = NULL;
  }

  MyFree(req);
}

/** Answer a request with a failure and release it.
 * @param[in] req Request.
 * @param[in] result What to tell the core.
 * @param[in] reason Text for the user, or NULL for the default.
 */
void ident_fail(struct IdentRequest* req, enum AccountResult result,
                const char* reason)
{
  account_id_t id = req->ir_id;
  enum IdentKind kind = req->ir_kind;
  int cancelled = req->ir_cancelled;

  ident_free(req);

  if (cancelled)
    return;

  switch (kind) {
  case IDENT_LOOKUP:
    /* Never FREE: see the note at the top of this file. */
    account_complete_owner(id, ACCOUNT_NICK_UNKNOWN);
    break;

  case IDENT_LIST:
    account_complete_list(id, result, NULL, 0, reason);
    break;

  default:
    account_complete(id, result, NULL, NULL, reason);
    break;
  }
}

/** Release every request without answering.  mi_fini only.
 *
 * By the time this runs the core has withdrawn the provider and failed
 * every question it was owed, and the database and cache have dropped this
 * module's callbacks; what is left is the bookkeeping.
 */
void ident_requests_clear(void)
{
  while (ident_requests)
    ident_free(ident_requests);
}

/** Forget a question.  The core has stopped caring about the answer.
 *
 * Marked rather than released: a query or a worker task is still on its
 * way back, and it is the callback that frees the request.  Answering is
 * what stops.
 *
 * @param[in] id Handle the core gave.
 */
void ident_cancel(account_id_t id)
{
  struct IdentRequest* req;

  for (req = ident_requests; req; req = req->ir_next)
    if (req->ir_id == id)
      req->ir_cancelled = 1;
}

/* ------------------------------------------------------------------- *
 * Helpers                                                             *
 * ------------------------------------------------------------------- */

/** Non-zero when there is a database to ask.
 * @param[in] what What was being attempted, for the log.
 */
int ident_db_ready(const char* what)
{
  if (db_available())
    return 1;

  log_write(LS_SYSTEM, L_ERROR, 0,
            "identity: no database driver, so %s cannot be answered", what);

  return 0;
}

/** Canonicalise a nickname the way the ircd compares them.
 *
 * ToLower(), not tolower(): in IRC '[', ']' and '\\' are the capitals of
 * '{', '}' and '|', so the C library's idea of the same nickname is not
 * the network's.
 *
 * @param[in] in Nickname as it was written.
 * @param[out] out Where the canonical form goes.
 * @param[in] len Size of \a out, terminator included.
 */
void ident_nick_canon(const char* in, char* out, size_t len)
{
  size_t i;

  if (!len)
    return;

  for (i = 0; in && in[i] && i + 1 < len; ++i)
    out[i] = ToLower(in[i]);

  out[i] = '\0';
}

/** Lower-case an address, ASCII only.
 *
 * Plain ASCII and not ToLower(): an address is not a nickname, and the
 * IRC case table would fold characters an address may legitimately carry.
 *
 * @param[in] in The address.
 * @param[out] out Where it goes.
 * @param[in] len Size of \a out.
 */
void ident_email_canon(const char* in, char* out, size_t len)
{
  size_t i;

  if (!len)
    return;

  for (i = 0; in && in[i] && i + 1 < len; ++i)
    out[i] = (in[i] >= 'A' && in[i] <= 'Z') ? (char) (in[i] - 'A' + 'a')
                                            : in[i];

  out[i] = '\0';
}

/** The server-wide pepper, or "" when there is none.
 * @param[out] len Its length, or NULL.
 */
const char* ident_pepper(size_t* len)
{
  const char* pepper = env_str(IDENT_PEPPER_ENV, "");
  size_t n;

  if (!pepper)
    pepper = "";

  n = strlen(pepper);

  if (n > IDENT_PEPPER_MAX)
    n = IDENT_PEPPER_MAX;

  if (len)
    *len = n;

  return pepper;
}

/** Fill in a worker payload's pepper.
 * @param[in,out] work Payload to fill in.
 */
void ident_pw_set_pepper(struct IdentPwWork* work)
{
  size_t len = 0;
  const char* pepper = ident_pepper(&len);

  memcpy(work->pw_pepper, pepper, len);
  work->pw_pepper[len] = '\0';
  work->pw_pepperlen = len;
}

/** Release a worker payload, wiping the password first.
 *
 * The default release would free it as it stands, which leaves a password
 * in whatever the allocator hands out next.
 *
 * @param[in] task The task being destroyed.
 */
void ident_pw_free(struct WorkTask* task)
{
  if (task->wt_in) {
    ircd_crypto_wipe(task->wt_in, sizeof(struct IdentPwWork));
    worker_free(task->wt_in);
    task->wt_in = NULL;
  }

  worker_free(task->wt_out);
  task->wt_out = NULL;
}

/* ------------------------------------------------------------------- *
 * Reading rows                                                        *
 * ------------------------------------------------------------------- */

/** One cell of a result, or NULL. */
static json_t* ident_cell(const struct DbResult* res, unsigned int row,
                          const char* column)
{
  json_t* data;
  json_t* object;

  if (!res || !(data = (json_t*) res->data) || !json_is_array(data))
    return NULL;

  if (!(object = json_array_get(data, row)) || !json_is_object(object))
    return NULL;

  return json_object_get(object, column);
}

/** How many rows a result carries. */
unsigned int ident_rows(const struct DbResult* res)
{
  json_t* data;

  if (!res || !(data = (json_t*) res->data) || !json_is_array(data))
    return 0;

  return (unsigned int) json_array_size(data);
}

/** A text column, or NULL when it is absent or SQL NULL. */
const char* ident_row_str(const struct DbResult* res, unsigned int row,
                          const char* column)
{
  json_t* value = ident_cell(res, row, column);

  if (!value || !json_is_string(value))
    return NULL;

  return json_string_value(value);
}

/** An integer column, or 0. */
long long ident_row_int(const struct DbResult* res, unsigned int row,
                        const char* column)
{
  json_t* value = ident_cell(res, row, column);

  if (!value)
    return 0;

  if (json_is_integer(value))
    return (long long) json_integer_value(value);

  if (json_is_string(value))
    return strtoll(json_string_value(value), NULL, 10);

  return 0;
}

/** A boolean column, or 0. */
int ident_row_bool(const struct DbResult* res, unsigned int row,
                   const char* column)
{
  json_t* value = ident_cell(res, row, column);

  if (!value)
    return 0;

  return json_is_true(value) ? 1 : 0;
}

/** Non-zero when the column is absent or SQL NULL. */
int ident_row_null(const struct DbResult* res, unsigned int row,
                   const char* column)
{
  json_t* value = ident_cell(res, row, column);

  return (!value || json_is_null(value)) ? 1 : 0;
}

/* ------------------------------------------------------------------- *
 * Lifecycle                                                           *
 * ------------------------------------------------------------------- */

/** Register the provider.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int identity_init(struct ModuleHandle* mod)
{
  ident_mod = mod;

  if (!module_add_account_provider(mod, &ident_provider)) {
    ident_mod = NULL;
    return -1;
  }

  /* Said once, at load, rather than once per failed login.  Neither is
   * fatal -- a server can be brought up before its database, and the
   * migrations have to be applied from somewhere -- but an operator who
   * forgot one of them should hear about it before the first user does.
   */
  if (!db_available())
    log_write(LS_SYSTEM, L_WARNING, 0,
              "identity: loaded with no database driver; nobody will be "
              "able to identify until one is loaded and Database{} is set");

  if (!cache_available())
    log_write(LS_SYSTEM, L_INFO, 0,
              "identity: loaded with no cache driver; every nickname "
              "lookup will reach the database");

  return 0;
}

/** Withdraw the provider and drop what is left.
 * @param[in] mod Handle for this module.
 */
static void identity_fini(struct ModuleHandle* mod)
{
  module_del_account_provider(mod);

  /* After the core has failed everything it was owed, so this frees
   * bookkeeping and no answer is lost. */
  ident_requests_clear();

  ident_mod = NULL;
}

/** What the loader reads. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "identity",
  "1.0",
  "ircu2",
  "answers whether a credential is good, and whose nickname is whose",
  identity_init,
  identity_fini,
  0
};
