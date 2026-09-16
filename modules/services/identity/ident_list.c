/*
 * IRC - Internet Relay Chat, modules/services/identity/ident_list.c
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
 * @brief "What accounts does this address hold?" -- ACCOUNT LIST.
 *
 * Not cached.  The nickname lookup is asked at every registration and
 * every nick change on the network; this one is asked when somebody types
 * ACCOUNT LIST, and a cache of it would be an invalidation rule to get
 * wrong in exchange for nothing.
 *
 * The address is never a search key the caller invented: m_account.c will
 * only pass the one in cli_user()->email, which is there because the
 * client authenticated with it.  This file does not have to check that
 * again, and could not: by the time the question arrives there is no
 * client attached to it.
 */
#include "config.h"

#include "db.h"
#include "identity.h"
#include "ircd_log.h"
#include "ircd_string.h"

#include <string.h>

/** Most accounts one answer will carry.
 *
 * The policy limit is nickserv's and is configurable; this is only the
 * size of the array the rows are copied into, generous enough that the
 * policy is what decides and a wrong query cannot make the server build
 * an unbounded reply.
 */
#define IDENT_LIST_MAX 32

/** The statement behind a listing.
 *
 * Ordered so that the answer does not depend on what the planner felt
 * like: the default first, then alphabetically.
 */
static const char ident_list_sql[] =
  "SELECT a.nick AS nick, a.is_default AS is_default"
  "  FROM account a"
  "  JOIN identity i ON i.id = a.identity_id"
  " WHERE i.email = $1"
  " ORDER BY a.is_default DESC, a.nick_canon";

/** Take the rows and answer.
 * @param[in] res The rows.
 * @param[in] user The request's serial.
 */
static void ident_list_rows(const struct DbResult* res, void* user)
{
  struct IdentRequest* req = ident_find((unsigned long) (size_t) user);
  struct AccountEntry entries[IDENT_LIST_MAX];
  char nicks[IDENT_LIST_MAX][NICKLEN + 1];
  unsigned int count = 0;
  unsigned int rows;
  unsigned int i;
  account_id_t id;
  int cancelled;

  if (!req)
    return;

  if (res->err.dberr_code != DB_OK) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: could not list the accounts of an address: %s",
              res->err.dberr_message);
    ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
    return;
  }

  rows = ident_rows(res);

  for (i = 0; i < rows && count < IDENT_LIST_MAX; ++i) {
    const char* nick = ident_row_str(res, i, "nick");

    if (EmptyString(nick))
      continue;

    /* Copied out of the result: it belongs to the driver and is released
     * as soon as this returns, and the entries outlive it by one call. */
    ircd_strncpy(nicks[count], nick, NICKLEN);
    entries[count].ae_nick = nicks[count];
    entries[count].ae_default = ident_row_bool(res, i, "is_default");
    count++;
  }

  id = req->ir_id;
  cancelled = req->ir_cancelled;
  ident_free(req);

  if (!cancelled)
    account_complete_list(id, ACCOUNT_OK, entries, count, NULL);
}

/** List the accounts an address holds.
 * @param[in] id Handle to answer with.
 * @param[in] email The address, already authenticated by the caller.
 */
void ident_list(account_id_t id, const char* email)
{
  struct IdentRequest* req = ident_begin(id, IDENT_LIST);
  struct DbParam address;
  struct DbParam* params[2];
  struct DbQuery query;
  enum DbError err;

  ident_email_canon(email, req->ir_email, sizeof(req->ir_email));

  if (!*req->ir_email) {
    ident_fail(req, ACCOUNT_ERR_CREDENTIAL, NULL);
    return;
  }

  if (!ident_db_ready("a listing")) {
    ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
    return;
  }

  address.type = DB_TYPE_TEXT;
  address.value = req->ir_email;
  address.format = DB_FORMAT_TEXT;

  params[0] = &address;
  params[1] = NULL;

  query.sql = ident_list_sql;
  query.params = params;

  err = db_query(ident_mod, &query, ident_list_rows,
                 (void*) (size_t) req->ir_serial);

  if (err != DB_OK) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "identity: could not ask for a listing: %s", db_strerror(err));
    ident_fail(req, ACCOUNT_ERR_UNAVAILABLE, NULL);
  }
}
