/*
 * IRC - Internet Relay Chat, modules/services/history/hist_store.c
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
 * @brief The statements.  See hist_store.h for why they are all here.
 */
#include "config.h"

#include "db.h"
#include "history.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/** How often a failed write may say so, in seconds.
 *
 * A database that is down fails one insert per message on the whole
 * network, so the log has to be rate-limited or it becomes the outage.
 */
#define HIST_LOG_EVERY 60

/** When the last write failure was reported. */
static time_t hist_last_complaint;
/** How many failures have gone unreported since. */
static unsigned int hist_suppressed;

/** Report a failed write, at most once a minute.
 * @param[in] what Which statement.
 * @param[in] res The result, or NULL when the call was refused outright.
 * @param[in] code The refusal, when \a res is NULL.
 */
static void hist_complain(const char* what, const struct DbResult* res,
                          enum DbError code)
{
  const char* msg;

  if (CurrentTime - hist_last_complaint < HIST_LOG_EVERY) {
    hist_suppressed++;
    return;
  }

  msg = res ? db_strerror(res->err.dberr_code) : db_strerror(code);

  if (hist_suppressed)
    log_write(LS_SYSTEM, L_ERROR, 0, "history: %s failed: %s (%s) "
              "(%u more since the last of these)", what, msg,
              res && res->err.dberr_message[0] ? res->err.dberr_message : "",
              hist_suppressed);
  else
    log_write(LS_SYSTEM, L_ERROR, 0, "history: %s failed: %s (%s)",
              what, msg,
              res && res->err.dberr_message[0] ? res->err.dberr_message : "");

  hist_last_complaint = CurrentTime;
  hist_suppressed = 0;
}

/** Callback for a write nobody is waiting on. */
static void hist_write_done(const struct DbResult* res, void* user)
{
  const char* what = (const char*) user;

  if (res->err.dberr_code != DB_OK)
    hist_complain(what, res, DB_OK);
}

/* ------------------------------------------------------------------- *
 * Writing                                                             *
 * ------------------------------------------------------------------- */

/** The insert.
 *
 * ON CONFLICT DO NOTHING is the whole deduplication story: every server
 * that delivered the message writes it, and the first one to arrive wins.
 * They are all writing the same row -- the identifier and the timestamp
 * both came from the message rather than from the server handling it --
 * so there is nothing to reconcile.
 */
static const char* hist_sql_insert =
  "INSERT INTO message (sent_at, msgid, kind, is_channel, target, "
  "target_canon, sender_nick, sender_account, recipient_account, body) "
  "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) "
  "ON CONFLICT (sent_at, msgid) DO NOTHING";

int hist_store_write(const struct HistMessage* msg)
{
  char kind[8];
  struct DbParam p_time = { DB_TYPE_TIMESTAMPTZ, 0, DB_FORMAT_TEXT };
  struct DbParam p_msgid = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_kind = { DB_TYPE_SMALLINT, kind, DB_FORMAT_TEXT };
  struct DbParam p_chan = { DB_TYPE_BOOL, 0, DB_FORMAT_TEXT };
  struct DbParam p_target = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_canon = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_sender = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_account = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_recip = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_body = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam* params[11];
  struct DbQuery query;
  enum DbError err;

  assert(0 != msg);

  /* A message with no name cannot be stored: there would be nothing to
   * deduplicate it by, so the same message would land once per server.
   * This does not happen -- the hook asks for the identifier, which
   * creates one -- but a row nobody can name is worse than no row.
   */
  if (!msg->hm_msgid || !msg->hm_msgid[0])
    return 0;

  ircd_snprintf(0, kind, sizeof(kind), "%d", (int) msg->hm_kind);

  p_time.value = msg->hm_time;
  p_msgid.value = msg->hm_msgid;
  p_chan.value = msg->hm_channel ? "true" : "false";
  p_target.value = msg->hm_target;
  p_canon.value = msg->hm_canon;
  p_sender.value = msg->hm_sender;
  p_account.value = msg->hm_account;   /* NULL is SQL NULL */
  p_recip.value = msg->hm_recipient;
  p_body.value = msg->hm_body ? msg->hm_body : "";

  params[0] = &p_time;
  params[1] = &p_msgid;
  params[2] = &p_kind;
  params[3] = &p_chan;
  params[4] = &p_target;
  params[5] = &p_canon;
  params[6] = &p_sender;
  params[7] = &p_account;
  params[8] = &p_recip;
  params[9] = &p_body;
  params[10] = NULL;

  query.sql = hist_sql_insert;
  query.params = params;

  err = db_exec(hist_mod, &query, hist_write_done, (void*) "write");

  if (err != DB_OK) {
    hist_complain("write", 0, err);
    return 0;
  }

  return 1;
}

/* ------------------------------------------------------------------- *
 * Partitions                                                          *
 * ------------------------------------------------------------------- */

/** Ask for one month's partition.
 * @param[in] when The month to create, as a time.
 */
static void hist_ensure_one(time_t when)
{
  char stamp[32];
  struct tm tm;
  struct DbParam p_ts = { DB_TYPE_TIMESTAMPTZ, stamp, DB_FORMAT_TEXT };
  struct DbParam* params[2];
  struct DbQuery query;
  enum DbError err;

  gmtime_r(&when, &tm);
  ircd_snprintf(0, stamp, sizeof(stamp), "%04d-%02d-01T00:00:00Z",
                tm.tm_year + 1900, tm.tm_mon + 1);

  params[0] = &p_ts;
  params[1] = NULL;

  query.sql = "SELECT history_ensure_partition($1)";
  query.params = params;

  /* db_exec and not db_query: it creates a table when it has to, and the
   * read side of a Database{} block may well be a replica. */
  err = db_exec(hist_mod, &query, hist_write_done, (void*) "partition");

  if (err != DB_OK)
    hist_complain("partition", 0, err);
}

void hist_store_ensure_partitions(int ahead)
{
  struct tm tm;
  time_t now = CurrentTime;
  int i;

  hist_ensure_one(now);

  for (i = 1; i <= ahead; i++) {
    time_t next;

    gmtime_r(&now, &tm);
    tm.tm_mday = 1;
    tm.tm_hour = 12;    /* mid-day, so no time zone slip can change the month */
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mon += i;
    next = timegm(&tm);

    if (next == (time_t) -1)
      break;

    hist_ensure_one(next);
  }
}

/* ------------------------------------------------------------------- *
 * Retention                                                           *
 * ------------------------------------------------------------------- */

/** What a purge came back with. */
static void hist_purge_done(const struct DbResult* res, void* user)
{
  long long dropped;

  if (res->err.dberr_code != DB_OK) {
    hist_complain("purge", res, DB_OK);
    return;
  }

  dropped = db_rows(res->data) ? db_row_int(res->data, 0, "history_purge") : 0;

  if (dropped > 0)
    log_write(LS_SYSTEM, L_INFO, 0,
              "history: purge dropped %lld monthly partition%s", dropped,
              dropped == 1 ? "" : "s");
}

void hist_store_purge(int days)
{
  char stamp[32];
  struct tm tm;
  time_t cutoff;
  struct DbParam p_before = { DB_TYPE_TIMESTAMPTZ, stamp, DB_FORMAT_TEXT };
  struct DbParam* params[2];
  struct DbQuery query;
  enum DbError err;

  /* Zero is "keep everything", and keeping everything is a decision an
   * operator makes rather than one this module makes for them. */
  if (days <= 0)
    return;

  cutoff = CurrentTime - (time_t) days * 24 * 60 * 60;
  gmtime_r(&cutoff, &tm);
  ircd_snprintf(0, stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);

  params[0] = &p_before;
  params[1] = NULL;

  query.sql = "SELECT history_purge($1)";
  query.params = params;

  err = db_exec(hist_mod, &query, hist_purge_done, 0);

  if (err != DB_OK)
    hist_complain("purge", 0, err);
}

/* ------------------------------------------------------------------- *
 * Forgetting                                                          *
 * ------------------------------------------------------------------- */

/** What a caller of hist_store_forget() is waiting on. */
struct HistForget {
  void (*hf_cb)(long long rows, void* user);
  void*  hf_user;
};

/** What a forget came back with. */
static void hist_forget_done(const struct DbResult* res, void* user)
{
  struct HistForget* hf = (struct HistForget*) user;
  long long rows = -1;

  if (res->err.dberr_code != DB_OK)
    hist_complain("forget", res, DB_OK);
  else if (db_rows(res->data))
    rows = db_row_int(res->data, 0, "history_forget");
  else
    rows = 0;

  if (hf->hf_cb)
    (hf->hf_cb)(rows, hf->hf_user);

  MyFree(hf);
}

int hist_store_forget(const char* account,
                      void (*cb)(long long rows, void* user), void* user)
{
  struct DbParam p_account = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam* params[2];
  struct DbQuery query;
  struct HistForget* hf;
  enum DbError err;

  assert(0 != account);

  p_account.value = account;
  params[0] = &p_account;
  params[1] = NULL;

  query.sql = "SELECT history_forget($1)";
  query.params = params;

  hf = (struct HistForget*) MyCalloc(1, sizeof(*hf));
  hf->hf_cb = cb;
  hf->hf_user = user;

  err = db_exec(hist_mod, &query, hist_forget_done, hf);

  if (err != DB_OK) {
    hist_complain("forget", 0, err);
    MyFree(hf);
    return 0;
  }

  return 1;
}
