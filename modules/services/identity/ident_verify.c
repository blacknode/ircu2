/*
 * IRC - Internet Relay Chat, modules/services/identity/ident_verify.c
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
 * @brief "Is this credential good?" -- one query, then one worker.
 *
 * The shape is the point.  Argon2 is deliberately slow and deliberately
 * hungry -- 50 to 250 ms and tens of megabytes -- so ten people logging in
 * at once would stop the server for a second if it were done here.  It is
 * not done here: the query goes to the database and comes back, the hash
 * goes to a worker thread and comes back, and the main thread waits for
 * neither.  ircd_pwhash_verify() is pure exactly so that this is possible.
 *
 * The row and the account are fetched in one query, before the password is
 * checked, and nothing about either is said until after it is: which
 * accounts an address holds is not something a wrong password gets to
 * find out.  For the same reason "no such address" and "wrong password"
 * are the same answer, #ACCOUNT_ERR_CREDENTIAL.
 *
 * When the stored hash was made with costs below what this server asks for
 * now, it is made again -- on a worker, and @em after the client has been
 * answered, so that raising the costs never shows up as login latency.
 * The update is guarded by the old hash, so a password changed in between
 * is not overwritten by a hash of the previous one.
 */
#include "config.h"

#include "db.h"
#include "identity.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "module.h"
#include "random.h"
#include "worker.h"

#include <string.h>

/** Address, no account named: take the default one. */
static const char ident_sql_email_default[] =
  "SELECT i.id AS identity_id, i.email AS email,"
  "       i.password_hash AS password_hash,"
  "       a.nick AS nick, (a.suspended_at IS NOT NULL) AS suspended"
  "  FROM identity i"
  "  LEFT JOIN account a ON a.identity_id = i.id AND a.is_default"
  " WHERE i.email = $1";

/** Address and a named account. */
static const char ident_sql_email_named[] =
  "SELECT i.id AS identity_id, i.email AS email,"
  "       i.password_hash AS password_hash,"
  "       a.nick AS nick, (a.suspended_at IS NOT NULL) AS suspended"
  "  FROM identity i"
  "  LEFT JOIN account a ON a.identity_id = i.id AND a.nick_canon = $2"
  " WHERE i.email = $1";

/** Certificate, no account named. */
static const char ident_sql_cert_default[] =
  "SELECT i.id AS identity_id, i.email AS email,"
  "       i.password_hash AS password_hash,"
  "       a.nick AS nick, (a.suspended_at IS NOT NULL) AS suspended"
  "  FROM identity i"
  "  LEFT JOIN account a ON a.identity_id = i.id AND a.is_default"
  " WHERE i.cert_fingerprint = $1";

/** Certificate and a named account. */
static const char ident_sql_cert_named[] =
  "SELECT i.id AS identity_id, i.email AS email,"
  "       i.password_hash AS password_hash,"
  "       a.nick AS nick, (a.suspended_at IS NOT NULL) AS suspended"
  "  FROM identity i"
  "  LEFT JOIN account a ON a.identity_id = i.id AND a.nick_canon = $2"
  " WHERE i.cert_fingerprint = $1";

/** Replace a hash that was made with costs below what is asked for now. */
static const char ident_sql_rehash[] =
  "UPDATE identity SET password_hash = $1"
  " WHERE id = $2 AND password_hash = $3";

/** Non-zero when \a mech proves itself with a certificate rather than a
 * password.
 * @param[in] mech Mechanism name.
 */
static int ident_is_external(const char* mech)
{
  return 0 == ircd_strcmp(mech, "EXTERNAL");
}

/** Answer a verification that has passed the credential check.
 *
 * Everything that is not about the password happens here, and only here,
 * so that there is one place where an account is turned down and one set
 * of reasons for it.
 *
 * @param[in] req The request, with the row already read.
 */
static void ident_verify_decide(struct IdentRequest* req)
{
  account_id_t id = req->ir_id;
  int cancelled = req->ir_cancelled;
  char account[NICKLEN + 1];
  char email[ACCOUNT_EMAIL_MAX + 1];
  int has_account = req->ir_has_account;
  int suspended = req->ir_suspended;

  ircd_strncpy(account, req->ir_account, NICKLEN);
  ircd_strncpy(email, req->ir_email, ACCOUNT_EMAIL_MAX);

  ident_free(req);

  if (cancelled)
    return;

  if (!has_account) {
    /* The address is real and the password was right; it just does not
     * hold the account that was asked for, or holds no default. */
    account_complete(id, ACCOUNT_ERR_NOSUCH, NULL, NULL, NULL);
    return;
  }

  if (suspended) {
    account_complete(id, ACCOUNT_ERR_SUSPENDED, NULL, NULL, NULL);
    return;
  }

  account_complete(id, ACCOUNT_OK, account, email, NULL);
}

/* ------------------------------------------------------------------- *
 * The worker                                                          *
 * ------------------------------------------------------------------- */

/** Argon2, in a worker thread.
 *
 * Obeys the one rule: no core state.  Everything it reads is in the task,
 * everything it writes is in the task, and it does not log, allocate with
 * MyMalloc() or look at CurrentTime.
 *
 * @param[in,out] task The work, with an IdentPwWork in WorkTask::wt_in.
 */
static void ident_pw_run(struct WorkTask* task)
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

  /* Only worth asking when the password was right: a re-hash needs the
   * password, and a wrong one is not it. */
  if (work->pw_ok)
    work->pw_outdated = ircd_pwhash_outdated(work->pw_hash, 0, 0);
}

/** Record a re-hash.  Main thread.
 * @param[in] res What the update did.
 * @param[in] user Unused.
 */
static void ident_rehash_stored(const struct DbResult* res, void* user)
{
  (void) user;

  if (res->err.dberr_code != DB_OK)
    log_write(LS_SYSTEM, L_WARNING, 0,
              "identity: could not store a re-hashed password: %s",
              res->err.dberr_message);
}

/** Store what the re-hash produced.  Main thread.
 * @param[in] task The finished work.
 */
static void ident_rehash_done(struct WorkTask* task)
{
  struct IdentPwWork* work = (struct IdentPwWork*) task->wt_in;
  struct DbParam fresh;
  struct DbParam row;
  struct DbParam previous;
  struct DbParam* params[4];
  struct DbQuery query;
  char id_text[32];

  if (!work->pw_ok || !*work->pw_newhash)
    return;

  ircd_snprintf(0, id_text, sizeof(id_text), "%lld", work->pw_identity_id);

  fresh.type = DB_TYPE_TEXT;
  fresh.value = work->pw_newhash;
  fresh.format = DB_FORMAT_TEXT;

  row.type = DB_TYPE_BIGINT;
  row.value = id_text;
  row.format = DB_FORMAT_TEXT;

  /* Guarded by the hash it is replacing.  A password changed between the
   * login and this update must not be overwritten by a re-hash of the old
   * one, and the row is the only thing that knows which came first. */
  previous.type = DB_TYPE_TEXT;
  previous.value = work->pw_hash;
  previous.format = DB_FORMAT_TEXT;

  params[0] = &fresh;
  params[1] = &row;
  params[2] = &previous;
  params[3] = NULL;

  query.sql = ident_sql_rehash;
  query.params = params;

  db_exec(ident_mod, &query, ident_rehash_stored, NULL);
}

/** Hash the password again with the costs this server asks for now.
 *
 * Called after the client has been answered, so that a network raising its
 * Argon2 costs does not make every login slower while the old hashes are
 * replaced.
 *
 * @param[in] old The payload of the verification that has just finished.
 */
static void ident_rehash(const struct IdentPwWork* old)
{
  struct IdentPwWork* work;
  struct WorkTask* task;
  unsigned int i;

  if (!(task = worker_task_new(ident_pw_run, ident_rehash_done)))
    return;

  task->wt_free = ident_pw_free;

  if (!(work = (struct IdentPwWork*) worker_alloc(sizeof(*work)))) {
    worker_task_free(task);
    return;
  }

  memcpy(work, old, sizeof(*work));
  work->pw_rehash = 1;
  work->pw_ok = 0;
  work->pw_outdated = 0;
  work->pw_serial = 0;
  work->pw_newhash[0] = '\0';

  /* The salt comes from the main thread: a worker may not touch core
   * state, and the random pool is core state. */
  for (i = 0; i < sizeof(work->pw_salt); ++i)
    work->pw_salt[i] = (unsigned char) (ircrandom() & 0xff);

  task->wt_in = work;

  if (!module_submit_work(ident_mod, task))
    worker_task_free(task);
}

/** Take what the worker decided.  Main thread.
 * @param[in] task The finished work.
 */
static void ident_pw_done(struct WorkTask* task)
{
  struct IdentPwWork* work = (struct IdentPwWork*) task->wt_in;
  struct IdentRequest* req = ident_find(work->pw_serial);
  int outdated;

  if (!req)
    return;

  /* The request handed its payload to the task when it submitted it. */
  req->ir_work = NULL;

  if (!work->pw_ok) {
    ident_fail(req, ACCOUNT_ERR_CREDENTIAL, NULL);
    return;
  }

  outdated = work->pw_outdated;

  ident_verify_decide(req);

  /* Answered first, then the housekeeping. */
  if (outdated)
    ident_rehash(work);
}

/* ------------------------------------------------------------------- *
 * The query                                                           *
 * ------------------------------------------------------------------- */

/** Hand the password and the stored hash to a worker.
 * @param[in,out] req The request, with the row read.
 * @param[in] stored The hash from the row.
 */
static void ident_verify_check(struct IdentRequest* req, const char* stored)
{
  struct WorkTask* task;

  if (EmptyString(stored) || !req->ir_work) {
    ident_fail(req, ACCOUNT_ERR_CREDENTIAL, NULL);
    return;
  }

  ircd_strncpy(req->ir_work->pw_hash, stored, PWHASH_MAX);
  req->ir_work->pw_identity_id = req->ir_identity_id;
  req->ir_work->pw_serial = req->ir_serial;

  if (!(task = worker_task_new(ident_pw_run, ident_pw_done))) {
    ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
    return;
  }

  task->wt_free = ident_pw_free;
  task->wt_in = req->ir_work;

  if (!module_submit_work(ident_mod, task)) {
    /* Worker threads are off, and hashing here would stop the server for
     * a quarter of a second per login.  Refusing is the honest failure:
     * the server is not configured to check passwords. */
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: no worker threads (FEAT_WORKER_THREADS is zero), "
              "so no password can be checked");
    worker_task_free(task);
    req->ir_work = NULL;   /* the task owned it and has just freed it */
    ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
    return;
  }

  /* The payload belongs to the task now, and the task to the server. */
  req->ir_work = NULL;
}

/** Take the row.
 * @param[in] res The rows.
 * @param[in] user The request's serial.
 */
static void ident_verify_rows(const struct DbResult* res, void* user)
{
  struct IdentRequest* req = ident_find((unsigned long) (size_t) user);
  const char* email;
  const char* nick;
  const char* stored;

  if (!req)
    return;

  if (res->err.dberr_code != DB_OK) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: could not check a credential: %s",
              res->err.dberr_message);
    ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
    return;
  }

  if (ident_rows(res) == 0) {
    /* No such address, or no certificate on file.  The same answer as a
     * wrong password, and for the same reason. */
    ident_fail(req, ACCOUNT_ERR_CREDENTIAL, NULL);
    return;
  }

  req->ir_identity_id = ident_row_int(res, 0, "identity_id");

  /* The address as it is stored, not as it was typed. */
  if ((email = ident_row_str(res, 0, "email")))
    ircd_strncpy(req->ir_email, email, ACCOUNT_EMAIL_MAX);

  if ((nick = ident_row_str(res, 0, "nick"))) {
    ircd_strncpy(req->ir_account, nick, NICKLEN);
    req->ir_has_account = 1;
    req->ir_suspended = ident_row_bool(res, 0, "suspended");
  }

  if (ident_is_external(req->ir_mech)) {
    /* The certificate is the credential, and the query matched on it.
     * There is nothing to hash. */
    ident_verify_decide(req);
    return;
  }

  stored = ident_row_str(res, 0, "password_hash");

  ident_verify_check(req, stored);
}

/** Check a credential.
 * @param[in] id Handle to answer with.
 * @param[in] areq What to check.  The core's; not kept.
 */
void ident_verify(account_id_t id, const struct AccountRequest* areq)
{
  struct IdentRequest* req = ident_begin(id, IDENT_VERIFY);
  struct DbParam subject;
  struct DbParam account;
  struct DbParam* params[3];
  struct DbQuery query;
  enum DbError err;
  int external;
  int named;

  ircd_strncpy(req->ir_mech, areq->ar_mech ? areq->ar_mech : "", SASLMECHLEN);
  ident_nick_canon(areq->ar_authzid ? areq->ar_authzid : "", req->ir_canon,
                   sizeof(req->ir_canon));
  ircd_strncpy(req->ir_authzid, areq->ar_authzid ? areq->ar_authzid : "",
               NICKLEN);

  external = ident_is_external(req->ir_mech);
  named = *req->ir_canon != '\0';

  if (external) {
    ircd_strncpy(req->ir_fingerprint,
                 areq->ar_fingerprint ? areq->ar_fingerprint : "",
                 sizeof(req->ir_fingerprint) - 1);

    if (!*req->ir_fingerprint) {
      ident_fail(req, ACCOUNT_ERR_CREDENTIAL, NULL);
      return;
    }

    subject.value = req->ir_fingerprint;
  } else {
    ident_email_canon(areq->ar_authcid, req->ir_email, sizeof(req->ir_email));

    if (!*req->ir_email || !areq->ar_secret || !areq->ar_secretlen
        || areq->ar_secretlen > SASL_SECRET_MAX) {
      ident_fail(req, ACCOUNT_ERR_CREDENTIAL, NULL);
      return;
    }

    /* The one copy this module makes of the password, and it is made
     * where it has to end up: in a buffer that can cross to a worker.
     * ident_free() wipes it if the request never gets that far. */
    req->ir_work = (struct IdentPwWork*) worker_alloc(sizeof(*req->ir_work));

    if (!req->ir_work) {
      ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
      return;
    }

    memset(req->ir_work, 0, sizeof(*req->ir_work));
    memcpy(req->ir_work->pw_secret, areq->ar_secret, areq->ar_secretlen);
    req->ir_work->pw_secret[areq->ar_secretlen] = '\0';
    req->ir_work->pw_secretlen = areq->ar_secretlen;
    ident_pw_set_pepper(req->ir_work);

    subject.value = req->ir_email;
  }

  if (!ident_db_ready("a credential")) {
    ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
    return;
  }

  subject.type = DB_TYPE_TEXT;
  subject.format = DB_FORMAT_TEXT;

  account.type = DB_TYPE_TEXT;
  account.value = req->ir_canon;
  account.format = DB_FORMAT_TEXT;

  params[0] = &subject;
  params[1] = named ? &account : NULL;
  params[2] = NULL;

  query.sql = external ? (named ? ident_sql_cert_named
                                : ident_sql_cert_default)
                       : (named ? ident_sql_email_named
                                : ident_sql_email_default);
  query.params = params;

  err = db_query(ident_mod, &query, ident_verify_rows,
                 (void*) (size_t) req->ir_serial);

  if (err != DB_OK) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: could not ask about a credential: %s",
              db_strerror(err));
    ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
  }
}
