/*
 * IRC - Internet Relay Chat, modules/services/identity/ident_change.c
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
 * @brief Writing: registering an account, changing a password, giving one up.
 *
 * Every write is the same three steps, which is why they are one file and
 * one entry point:
 *
 *   1. read the address's row;
 *   2. an Argon2 hop on a worker -- checking the password that was given,
 *      and for a new address or a new password, making one;
 *   3. one statement that changes the store.
 *
 * Step 3 is a @b function and not a statement this module composes, and
 * that is the whole design.  Counting an address's accounts and then
 * inserting one is a race the moment there are two servers: both count
 * two, both insert, the address ends up with four.  The count and the
 * insert have to be one transaction that begins by locking the address,
 * and db.h has no transactions -- it runs one statement on a pooled
 * connection, which is the right shape for everything else it does.  A
 * statement sent on its own runs in an implicit transaction, so
 * @c pg_advisory_xact_lock() taken inside a function called by one
 * statement is held for exactly that statement.  The locking therefore
 * lives in the migration, beside the tables it protects; see
 * migrations/v2_account_writes.up.sql.
 *
 * The unique indexes are still there and are not the defence: they are
 * what catches somebody inserting by hand, which no lock can.
 *
 * Every successful write forgets the nickname's cache entry rather than
 * waiting for the TTL, because the store is shared and one delete here is
 * a delete for the whole network.
 */
#include "config.h"

#include "cache.h"
#include "db.h"
#include "identity.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "module.h"
#include "random.h"
#include "worker.h"

#include <string.h>

/** The address's row, and nothing else: the password is checked before
 * anything about the address is acted on. */
static const char ident_sql_identity[] =
  "SELECT i.id AS identity_id, i.password_hash AS password_hash"
  "  FROM identity i WHERE i.email = $1";

/** Take a nickname for an address, creating the address if it is new. */
static const char ident_sql_register[] =
  "SELECT status, account, is_new"
  "  FROM account_register($1, $2, $3, $4, $5)";

/** Give a nickname up. */
static const char ident_sql_drop[] =
  "SELECT status, account FROM account_drop($1, $2)";

/** Replace a password, guarded by the one it replaces. */
static const char ident_sql_passwd[] =
  "UPDATE identity SET password_hash = $1"
  "  WHERE email = $2 AND password_hash = $3";

/** Answer a change and release the request.
 * @param[in] req The request.
 * @param[in] result What to tell the core.
 */
static void ident_change_answer(struct IdentRequest* req,
                                enum AccountResult result)
{
  account_id_t id = req->ir_id;
  int cancelled = req->ir_cancelled;
  char account[NICKLEN + 1];
  char email[ACCOUNT_EMAIL_MAX + 1];

  ircd_strncpy(account, req->ir_account, NICKLEN);
  ircd_strncpy(email, req->ir_email, ACCOUNT_EMAIL_MAX);

  ident_free(req);

  if (cancelled)
    return;

  if (result == ACCOUNT_OK)
    account_complete(id, result, *account ? account : NULL, email, NULL);
  else
    account_complete(id, result, NULL, NULL, NULL);
}

/** Turn the status a write function returned into a result.
 * @param[in] status The status column, or NULL.
 */
static enum AccountResult ident_change_status(const char* status)
{
  if (EmptyString(status))
    return ACCOUNT_ERR_UNAVAILABLE;

  if (0 == strcmp(status, "ok"))
    return ACCOUNT_OK;

  if (0 == strcmp(status, "exists"))
    return ACCOUNT_ERR_EXISTS;

  if (0 == strcmp(status, "limit"))
    return ACCOUNT_ERR_LIMIT;

  if (0 == strcmp(status, "nosuch"))
    return ACCOUNT_ERR_NOSUCH;

  return ACCOUNT_ERR_UNAVAILABLE;
}

/** Take what a write did.
 * @param[in] res The rows.
 * @param[in] user The request's serial.
 */
static void ident_change_written(const struct DbResult* res, void* user)
{
  struct IdentRequest* req = ident_find((unsigned long) (size_t) user);
  enum AccountResult result;
  const char* account;

  if (!req)
    return;

  if (res->err.dberr_code != DB_OK) {
    log_write(LS_SYSTEM, L_ERROR, 0, "identity: a write failed: %s",
              res->err.dberr_message);
    ident_change_answer(req, ACCOUNT_ERR_UNAVAILABLE);
    return;
  }

  if (req->ir_what == ACCOUNT_WRITE_PASSWD) {
    /* An UPDATE, not a function: no status column, and no rows means the
     * stored hash is not the one that was verified a moment ago, which is
     * somebody else having changed the password in between. */
    ident_change_answer(req, res->rows ? ACCOUNT_OK : ACCOUNT_ERR_CREDENTIAL);
    return;
  }

  if (ident_rows(res) == 0) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: a write returned nothing; are the migrations "
              "applied?");
    ident_change_answer(req, ACCOUNT_ERR_UNAVAILABLE);
    return;
  }

  result = ident_change_status(ident_row_str(res, 0, "status"));

  if (result == ACCOUNT_OK) {
    if ((account = ident_row_str(res, 0, "account")))
      ircd_strncpy(req->ir_account, account, NICKLEN);

    /* The store is shared, so this is a delete for every server. */
    ident_forget_nick(req->ir_canon);
  }

  ident_change_answer(req, result);
}

/** Send the statement that makes the change.
 * @param[in,out] req The request, with everything it needs read.
 * @param[in] hash The hash to store, or NULL for the writes that keep one.
 */
static void ident_change_write(struct IdentRequest* req, const char* hash)
{
  struct DbParam p[5];
  struct DbParam* params[6];
  struct DbQuery query;
  char max_text[16];
  enum DbError err;
  unsigned int n = 0;

  memset(p, 0, sizeof(p));

  switch (req->ir_what) {
  case ACCOUNT_WRITE_REGISTER:
    ircd_snprintf(0, max_text, sizeof(max_text), "%d", req->ir_max);

    p[0].type = DB_TYPE_TEXT;   p[0].value = req->ir_email;
    p[1].type = DB_TYPE_TEXT;   p[1].value = hash ? hash : "";
    p[2].type = DB_TYPE_TEXT;   p[2].value = req->ir_account;
    p[3].type = DB_TYPE_TEXT;   p[3].value = req->ir_canon;
    p[4].type = DB_TYPE_INT;    p[4].value = max_text;
    n = 5;
    query.sql = ident_sql_register;
    break;

  case ACCOUNT_WRITE_DROP:
    p[0].type = DB_TYPE_TEXT;   p[0].value = req->ir_email;
    p[1].type = DB_TYPE_TEXT;   p[1].value = req->ir_canon;
    n = 2;
    query.sql = ident_sql_drop;
    break;

  default:
    p[0].type = DB_TYPE_TEXT;   p[0].value = hash ? hash : "";
    p[1].type = DB_TYPE_TEXT;   p[1].value = req->ir_email;
    p[2].type = DB_TYPE_TEXT;   p[2].value = req->ir_work
                                             ? req->ir_work->pw_hash : "";
    n = 3;
    query.sql = ident_sql_passwd;
    break;
  }

  for (params[n] = NULL; n--; )
    params[n] = &p[n];

  query.params = params;

  err = db_exec(ident_mod, &query, ident_change_written,
                (void*) (size_t) req->ir_serial);

  if (err != DB_OK) {
    log_write(LS_SYSTEM, L_ERROR, 0, "identity: could not write: %s",
              db_strerror(err));
    ident_change_answer(req, ACCOUNT_ERR_UNAVAILABLE);
  }
}

/* ------------------------------------------------------------------- *
 * The worker hops                                                     *
 * ------------------------------------------------------------------- */

static void ident_change_run(struct WorkTask* task);

/** Hand a payload to the pool.
 * @param[in,out] req The request it belongs to.
 * @param[in,out] work The payload; owned by the task afterwards.
 * @param[in] done What to run in the main thread.
 * @return Non-zero if the pool took it.
 */
static int ident_change_submit(struct IdentRequest* req,
                               struct IdentPwWork* work, WorkDoneFn done)
{
  struct WorkTask* task;

  work->pw_serial = req->ir_serial;

  if (!(task = worker_task_new(ident_change_run, done)))
    return 0;

  task->wt_free = ident_pw_free;
  task->wt_in = work;

  if (!module_submit_work(ident_mod, task)) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: no worker threads (FEAT_WORKER_THREADS is zero), "
              "so no password can be hashed");
    worker_task_free(task);
    return 0;
  }

  return 1;
}

/** Argon2, in a worker thread.  See ident_verify.c for the one rule. */
static void ident_change_run(struct WorkTask* task)
{
  struct IdentPwWork* work = (struct IdentPwWork*) task->wt_in;
  const void* pepper = work->pw_pepperlen ? work->pw_pepper : NULL;

  if (work->pw_rehash) {
    work->pw_ok = ircd_pwhash_make(work->pw_newhash, work->pw_secret,
                                   work->pw_secretlen, work->pw_salt,
                                   sizeof(work->pw_salt), pepper,
                                   work->pw_pepperlen, 0, 0);
    return;
  }

  work->pw_ok = ircd_pwhash_verify(work->pw_hash, work->pw_secret,
                                   work->pw_secretlen, pepper,
                                   work->pw_pepperlen);
}

/** Start hashing the password that is being set.
 * @param[in,out] req The request; its ir_make is handed on.
 * @param[in] done What to run with the answer.
 */
static void ident_change_hash(struct IdentRequest* req, WorkDoneFn done)
{
  struct IdentPwWork* work = req->ir_make;
  unsigned int i;

  if (!work) {
    ident_change_answer(req, ACCOUNT_ERR_CREDENTIAL);
    return;
  }

  work->pw_rehash = 1;

  /* The salt comes from the main thread: a worker may not touch core
   * state, and the random pool is core state. */
  for (i = 0; i < sizeof(work->pw_salt); ++i)
    work->pw_salt[i] = (unsigned char) (ircrandom() & 0xff);

  req->ir_make = NULL;

  if (!ident_change_submit(req, work, done))
    ident_change_answer(req, ACCOUNT_ERR_UNAVAILABLE);
}

/** The new password has been hashed; write it.  Main thread. */
static void ident_change_hashed(struct WorkTask* task)
{
  struct IdentPwWork* work = (struct IdentPwWork*) task->wt_in;
  struct IdentRequest* req = ident_find(work->pw_serial);

  if (!req)
    return;

  if (!work->pw_ok) {
    log_write(LS_SYSTEM, L_ERROR, 0, "identity: could not hash a password");
    ident_change_answer(req, ACCOUNT_ERR_UNAVAILABLE);
    return;
  }

  ident_change_write(req, work->pw_newhash);
}

/** The password that was given has been checked.  Main thread. */
static void ident_change_verified(struct WorkTask* task)
{
  struct IdentPwWork* work = (struct IdentPwWork*) task->wt_in;
  struct IdentRequest* req = ident_find(work->pw_serial);

  if (!req)
    return;

  if (!work->pw_ok) {
    ident_change_answer(req, ACCOUNT_ERR_CREDENTIAL);
    return;
  }

  /* Changing a password is the one write that has a second password to
   * hash; the others already have everything they need. */
  if (req->ir_what == ACCOUNT_WRITE_PASSWD) {
    ident_change_hash(req, ident_change_hashed);
    return;
  }

  ident_change_write(req, work->pw_hash);
}

/* ------------------------------------------------------------------- *
 * The read                                                            *
 * ------------------------------------------------------------------- */

/** Take the address's row.
 * @param[in] res The rows.
 * @param[in] user The request's serial.
 */
static void ident_change_row(const struct DbResult* res, void* user)
{
  struct IdentRequest* req = ident_find((unsigned long) (size_t) user);
  const char* stored;

  if (!req)
    return;

  if (res->err.dberr_code != DB_OK) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: could not read an address before writing: %s",
              res->err.dberr_message);
    ident_change_answer(req, ACCOUNT_ERR_UNAVAILABLE);
    return;
  }

  if (ident_rows(res) == 0) {
    /* No such address.  Registering makes it -- that is what registering
     * is -- and the password given is the one being set.  Anything else
     * has nothing to change and no way to tell an address that does not
     * exist from one whose password is wrong, which is the same answer
     * either way.
     */
    if (req->ir_what != ACCOUNT_WRITE_REGISTER) {
      ident_change_answer(req, ACCOUNT_ERR_CREDENTIAL);
      return;
    }

    /* The password given becomes the address's password. */
    if (!req->ir_make) {
      req->ir_make = req->ir_work;
      req->ir_work = NULL;
    }

    ident_change_hash(req, ident_change_hashed);
    return;
  }

  req->ir_identity_id = ident_row_int(res, 0, "identity_id");
  stored = ident_row_str(res, 0, "password_hash");

  if (EmptyString(stored) || !req->ir_work) {
    ident_change_answer(req, ACCOUNT_ERR_CREDENTIAL);
    return;
  }

  /* The address exists, so the password given has to be its password --
   * registering a second nickname for an address is as much an act of its
   * holder as changing the password is.
   */
  ircd_strncpy(req->ir_work->pw_hash, stored, PWHASH_MAX);

  {
    struct IdentPwWork* work = req->ir_work;

    req->ir_work = NULL;

    if (!ident_change_submit(req, work, ident_change_verified)) {
      /* The task owns the payload and has already freed it. */
      ident_change_answer(req, ACCOUNT_ERR_UNAVAILABLE);
      return;
    }

    /* Kept for the guard on an UPDATE; the payload is the task's now, so
     * this is a copy and not a pointer into it. */
    if (req->ir_what == ACCOUNT_WRITE_PASSWD) {
      req->ir_work = (struct IdentPwWork*) worker_alloc(sizeof(*req->ir_work));

      if (req->ir_work) {
        memset(req->ir_work, 0, sizeof(*req->ir_work));
        ircd_strncpy(req->ir_work->pw_hash, stored, PWHASH_MAX);
      }
    }
  }
}

/** Make a change to the store.
 * @param[in] id Handle to answer with.
 * @param[in] creq What to change.  The core's; not kept.
 */
void ident_change(account_id_t id, const struct AccountChange* creq)
{
  struct IdentRequest* req = ident_begin(id, IDENT_CHANGE);
  struct DbParam address;
  struct DbParam* params[2];
  struct DbQuery query;
  enum DbError err;

  req->ir_what = creq->ach_what;
  req->ir_max = creq->ach_max;

  ident_email_canon(creq->ach_email, req->ir_email, sizeof(req->ir_email));
  ircd_strncpy(req->ir_account, creq->ach_nick ? creq->ach_nick : "", NICKLEN);
  ident_nick_canon(req->ir_account, req->ir_canon, sizeof(req->ir_canon));

  if (!*req->ir_email) {
    ident_fail(req, ACCOUNT_ERR_CREDENTIAL, NULL);
    return;
  }

  if (req->ir_what != ACCOUNT_WRITE_PASSWD && !*req->ir_canon) {
    ident_fail(req, ACCOUNT_ERR_NOSUCH, NULL);
    return;
  }

  req->ir_work = ident_pw_new(creq->ach_secret, creq->ach_secretlen);

  if (!req->ir_work) {
    ident_fail(req, ACCOUNT_ERR_CREDENTIAL, NULL);
    return;
  }

  if (req->ir_what == ACCOUNT_WRITE_PASSWD) {
    req->ir_make = ident_pw_new(creq->ach_new, creq->ach_newlen);

    if (!req->ir_make) {
      ident_fail(req, ACCOUNT_ERR_CREDENTIAL, NULL);
      return;
    }
  }

  if (!ident_db_ready("a change")) {
    ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
    return;
  }

  address.type = DB_TYPE_TEXT;
  address.value = req->ir_email;
  address.format = DB_FORMAT_TEXT;

  params[0] = &address;
  params[1] = NULL;

  query.sql = ident_sql_identity;
  query.params = params;

  /* Read on the write side: what comes next is a write, and a primary and
   * a replica disagreeing for a moment is how an address gets two
   * password changes that each think they were first.
   */
  err = db_exec(ident_mod, &query, ident_change_row,
                (void*) (size_t) req->ir_serial);

  if (err != DB_OK) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: could not read an address: %s", db_strerror(err));
    ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
  }
}
