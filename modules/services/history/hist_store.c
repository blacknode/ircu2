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

/** Non-zero once the schema has answered a statement of ours. */
static int hist_ready;

int hist_store_ready(void)
{
  return hist_ready;
}

/** Callback for a write nobody is waiting on. */
static void hist_write_done(const struct DbResult* res, void* user)
{
  const char* what = (const char*) user;

  if (res->err.dberr_code != DB_OK) {
    hist_complain(what, res, DB_OK);
    return;
  }

  /* The partition statement is the one that says the schema is really
   * there: it names a function the migrations create.  Until it has
   * worked once, the module keeps looking. */
  if (!strcmp(what, "partition"))
    hist_ready = 1;
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
  "target_canon, sender_nick, sender_account, recipient_account, body, "
  "sender_prefix, reply_to) "
  "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12) "
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
  struct DbParam p_prefix = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_reply = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam* params[13];
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
  p_prefix.value = msg->hm_prefix;
  p_reply.value = msg->hm_reply;   /* NULL is SQL NULL */

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
  params[10] = &p_prefix;
  params[11] = &p_reply;
  params[12] = NULL;

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

/* ------------------------------------------------------------------- *
 * Reading                                                             *
 * ------------------------------------------------------------------- */

/** The columns every read returns, in one place so they cannot drift.
 *
 * The timestamp comes back as ISO 8601 with milliseconds and an explicit
 * Z, which is both what the @c time tag has to carry and what a client
 * sends as a selector: the same text goes out, comes back and is compared,
 * so nothing in this module ever parses a date, and a plain strcmp()
 * orders two of them -- which is what tells BETWEEN's two points apart.
 */
#define HIST_COLUMNS \
  "to_char(sent_at AT TIME ZONE 'UTC', " \
  "'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS sent_at, " \
  "msgid, kind, target, body, " \
  "coalesce(sender_prefix, sender_nick) AS prefix, " \
  "coalesce(reply_to, '') AS reply_to"

/** One read, from the moment it is accepted until its rows are handed on.
 *
 * Held rather than passed on the stack because a request that names a
 * point by message takes two round trips: the identifier is resolved to
 * its (time, message) pair, and only then is the real statement built.
 */
struct HistRead {
  struct HistQuery hr_q;      /**< What was asked, points filled in. */
  HistReadFn       hr_cb;     /**< Who to tell. */
  void*            hr_user;   /**< What to tell them with. */
  int              hr_resolving; /**< Waiting on the identifier lookup. */
};

/** Tell the caller nothing came back, and let the read go. */
static void hist_read_fail(struct HistRead* rd)
{
  if (rd->hr_cb)
    (rd->hr_cb)(0, 0, 0, rd->hr_user);
  MyFree(rd);
}

/** db_row_str() hands back a buffer good until the next call, so the rows
 * above cannot all point into it.  This copies each cell into the array's
 * own storage instead.
 */
struct HistCell {
  char hc_time[40];
  char hc_msgid[MSGIDLEN + 1];
  char hc_target[CHANNELLEN + 1];
  char hc_prefix[NICKLEN + USERLEN + HOSTLEN + 3];
  char hc_body[BUFSIZE];
  char hc_from[NICKLEN + 1];
  char hc_to[NICKLEN + 1];
  char hc_reply[MSGIDLEN + 1];
};

/** What came back from a read. */
static void hist_read_done(const struct DbResult* res, void* user)
{
  struct HistRead* rd = (struct HistRead*) user;
  struct HistRow* rows;
  struct HistCell* cells;
  unsigned int count;
  unsigned int i;
  int reverse;

  if (res->err.dberr_code != DB_OK) {
    hist_complain("read", res, DB_OK);
    hist_read_fail(rd);
    return;
  }

  /* LATEST and BEFORE read backwards to find the newest N; everything
   * else already comes out oldest first. */
  reverse = (rd->hr_q.hq_shape == HIST_LATEST
             || rd->hr_q.hq_shape == HIST_BEFORE);

  count = db_rows(res->data);

  if (!count) {
    if (rd->hr_cb)
      (rd->hr_cb)(1, 0, 0, rd->hr_user);
    MyFree(rd);
    return;
  }

  rows = (struct HistRow*) MyCalloc(count, sizeof(*rows));
  cells = (struct HistCell*) MyCalloc(count, sizeof(*cells));

  for (i = 0; i < count; i++) {
    unsigned int from = reverse ? (count - 1 - i) : i;

    ircd_strncpy(cells[i].hc_time, db_row_str(res->data, from, "sent_at"),
                 sizeof(cells[i].hc_time) - 1);
    ircd_strncpy(cells[i].hc_msgid, db_row_str(res->data, from, "msgid"),
                 sizeof(cells[i].hc_msgid) - 1);
    ircd_strncpy(cells[i].hc_target, db_row_str(res->data, from, "target"),
                 sizeof(cells[i].hc_target) - 1);
    ircd_strncpy(cells[i].hc_prefix, db_row_str(res->data, from, "prefix"),
                 sizeof(cells[i].hc_prefix) - 1);
    ircd_strncpy(cells[i].hc_body, db_row_str(res->data, from, "body"),
                 sizeof(cells[i].hc_body) - 1);
    ircd_strncpy(cells[i].hc_reply, db_row_str(res->data, from, "reply_to"),
                 sizeof(cells[i].hc_reply) - 1);

    rows[i].hr_time = cells[i].hc_time;
    rows[i].hr_msgid = cells[i].hc_msgid;
    rows[i].hr_kind = (enum HistKind) db_row_int(res->data, from, "kind");
    rows[i].hr_target = cells[i].hc_target;
    rows[i].hr_prefix = cells[i].hc_prefix;
    rows[i].hr_body = cells[i].hc_body;
    rows[i].hr_reply = cells[i].hc_reply;
  }

  if (rd->hr_cb)
    (rd->hr_cb)(1, rows, count, rd->hr_user);

  MyFree(cells);
  MyFree(rows);
  MyFree(rd);
}

/** Build the WHERE clause that names the conversation.
 *
 * A channel is one column.  A conversation is two, either way round,
 * which is why there are two indexes for it: the query has to be an OR
 * and an OR of unindexed columns reads the month.
 *
 * @param[in] q What is being asked.
 * @param[out] buf Where to write it.
 * @param[in] buflen Size of \a buf.
 * @param[in] first The number of the first free $n placeholder.
 * @return How many placeholders were used.
 */
static int hist_where_target(const struct HistQuery* q, char* buf,
                             size_t buflen, int first)
{
  if (q->hq_channel) {
    ircd_snprintf(0, buf, buflen, "is_channel AND target_canon = $%d", first);
    return 1;
  }

  ircd_snprintf(0, buf, buflen,
                "NOT is_channel AND ((sender_account = $%d AND "
                "recipient_account = $%d) OR (sender_account = $%d AND "
                "recipient_account = $%d))",
                first, first + 1, first + 1, first);
  return 2;
}

/** Run the statement a request has been resolved into. */
static void hist_read_run(struct HistRead* rd)
{
  const struct HistQuery* q = &rd->hr_q;
  char target[512];
  char sql[2048];
  char limit[16];
  char half[16];
  struct DbParam p[8];
  struct DbParam* params[9];
  struct DbQuery query;
  enum DbError err;
  int n = 0;        /* parameters bound so far */
  int used;
  int i;

  used = hist_where_target(q, target, sizeof(target), 1);

  if (q->hq_channel) {
    p[n].type = DB_TYPE_TEXT; p[n].value = q->hq_canon;
    p[n].format = DB_FORMAT_TEXT; n++;
  } else {
    p[n].type = DB_TYPE_TEXT; p[n].value = q->hq_self;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_TEXT; p[n].value = q->hq_peer;
    p[n].format = DB_FORMAT_TEXT; n++;
  }

  ircd_snprintf(0, limit, sizeof(limit), "%u", q->hq_limit);
  ircd_snprintf(0, half, sizeof(half), "%u", (q->hq_limit + 1) / 2);

  switch (q->hq_shape) {
  case HIST_LATEST:
    ircd_snprintf(0, sql, sizeof(sql),
                  "SELECT " HIST_COLUMNS " FROM message WHERE %s "
                  "ORDER BY sent_at DESC, msgid DESC LIMIT $%d",
                  target, used + 1);
    p[n].type = DB_TYPE_INT; p[n].value = limit;
    p[n].format = DB_FORMAT_TEXT; n++;
    break;

  case HIST_BEFORE:
    ircd_snprintf(0, sql, sizeof(sql),
                  "SELECT " HIST_COLUMNS " FROM message WHERE %s "
                  "AND (sent_at, msgid) < ($%d, $%d) "
                  "ORDER BY sent_at DESC, msgid DESC LIMIT $%d",
                  target, used + 1, used + 2, used + 3);
    p[n].type = DB_TYPE_TIMESTAMPTZ; p[n].value = q->hq_a.hp_time;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_TEXT; p[n].value = q->hq_a.hp_msgid;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_INT; p[n].value = limit;
    p[n].format = DB_FORMAT_TEXT; n++;
    break;

  case HIST_AFTER:
    ircd_snprintf(0, sql, sizeof(sql),
                  "SELECT " HIST_COLUMNS " FROM message WHERE %s "
                  "AND (sent_at, msgid) > ($%d, $%d) "
                  "ORDER BY sent_at ASC, msgid ASC LIMIT $%d",
                  target, used + 1, used + 2, used + 3);
    p[n].type = DB_TYPE_TIMESTAMPTZ; p[n].value = q->hq_a.hp_time;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_TEXT; p[n].value = q->hq_a.hp_msgid;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_INT; p[n].value = limit;
    p[n].format = DB_FORMAT_TEXT; n++;
    break;

  case HIST_AROUND:
    /* Half either side, and the point itself counts as being before it:
     * a client asking for context around a message expects that message
     * in the answer. */
    ircd_snprintf(0, sql, sizeof(sql),
                  "(SELECT " HIST_COLUMNS " FROM message WHERE %s "
                  "AND (sent_at, msgid) <= ($%d, $%d) "
                  "ORDER BY sent_at DESC, msgid DESC LIMIT $%d) "
                  "UNION ALL "
                  "(SELECT " HIST_COLUMNS " FROM message WHERE %s "
                  "AND (sent_at, msgid) > ($%d, $%d) "
                  "ORDER BY sent_at ASC, msgid ASC LIMIT $%d) "
                  "ORDER BY sent_at ASC, msgid ASC",
                  target, used + 1, used + 2, used + 3,
                  target, used + 1, used + 2, used + 3);
    p[n].type = DB_TYPE_TIMESTAMPTZ; p[n].value = q->hq_a.hp_time;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_TEXT; p[n].value = q->hq_a.hp_msgid;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_INT; p[n].value = half;
    p[n].format = DB_FORMAT_TEXT; n++;
    break;

  case HIST_BETWEEN:
    /* Symmetric on purpose.  The two points may arrive either way round,
     * and which one is earlier is a question only the database can answer:
     * it is the one comparing the pairs, under its own collation, and a
     * decision made here in C would disagree with it for two messages that
     * share a second and differ only in case.  Exactly one of the two
     * branches can match anything, and each is a range the index serves.
     */
    ircd_snprintf(0, sql, sizeof(sql),
                  "SELECT " HIST_COLUMNS " FROM message WHERE %s "
                  "AND (((sent_at, msgid) > ($%d, $%d) "
                  "AND (sent_at, msgid) < ($%d, $%d)) "
                  "OR ((sent_at, msgid) > ($%d, $%d) "
                  "AND (sent_at, msgid) < ($%d, $%d))) "
                  "ORDER BY sent_at ASC, msgid ASC LIMIT $%d",
                  target,
                  used + 1, used + 2, used + 3, used + 4,
                  used + 3, used + 4, used + 1, used + 2,
                  used + 5);
    p[n].type = DB_TYPE_TIMESTAMPTZ; p[n].value = q->hq_a.hp_time;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_TEXT; p[n].value = q->hq_a.hp_msgid;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_TIMESTAMPTZ; p[n].value = q->hq_b.hp_time;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_TEXT; p[n].value = q->hq_b.hp_msgid;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_INT; p[n].value = limit;
    p[n].format = DB_FORMAT_TEXT; n++;
    break;

  case HIST_TARGETS:
    /* Conversations, not channels: a client already knows which channels
     * it is on, and the ones it is not on it may not read. */
    ircd_snprintf(0, sql, sizeof(sql),
                  "SELECT peer AS target, "
                  "to_char(max(sent_at) AT TIME ZONE 'UTC', "
                  "'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS sent_at, "
                  "'' AS msgid, 0 AS kind, '' AS body, '' AS prefix, "
                  "'' AS reply_to "
                  "FROM (SELECT CASE WHEN sender_account = $1 "
                  "THEN recipient_account ELSE sender_account END AS peer, "
                  "sent_at FROM message WHERE NOT is_channel "
                  "AND (sender_account = $1 OR recipient_account = $1) "
                  "AND sent_at > $2 AND sent_at < $3) c "
                  "GROUP BY peer ORDER BY 2 ASC LIMIT $4");
    n = 0;
    p[n].type = DB_TYPE_TEXT; p[n].value = q->hq_self;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_TIMESTAMPTZ; p[n].value = q->hq_a.hp_time;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_TIMESTAMPTZ; p[n].value = q->hq_b.hp_time;
    p[n].format = DB_FORMAT_TEXT; n++;
    p[n].type = DB_TYPE_INT; p[n].value = limit;
    p[n].format = DB_FORMAT_TEXT; n++;
    break;

  default:
    hist_read_fail(rd);
    return;
  }

  for (i = 0; i < n; i++)
    params[i] = &p[i];
  params[n] = NULL;

  query.sql = sql;
  query.params = params;

  err = db_query(hist_mod, &query, hist_read_done, rd);

  if (err != DB_OK) {
    hist_complain("read", 0, err);
    hist_read_fail(rd);
  }
}

/** Fill in a point's timestamp from the message it named. */
static void hist_resolve_done(const struct DbResult* res, void* user)
{
  struct HistRead* rd = (struct HistRead*) user;
  unsigned int rows;
  unsigned int i;

  if (res->err.dberr_code != DB_OK) {
    hist_complain("resolve", res, DB_OK);
    hist_read_fail(rd);
    return;
  }

  rows = db_rows(res->data);

  for (i = 0; i < rows; i++) {
    const char* id = db_row_str(res->data, i, "msgid");
    char when[40];

    ircd_strncpy(when, db_row_str(res->data, i, "sent_at"), sizeof(when) - 1);
    when[sizeof(when) - 1] = '\0';

    if (rd->hr_q.hq_a.hp_msgid[0] && !strcmp(id, rd->hr_q.hq_a.hp_msgid))
      ircd_strncpy(rd->hr_q.hq_a.hp_time, when,
                   sizeof(rd->hr_q.hq_a.hp_time) - 1);
    if (rd->hr_q.hq_b.hp_msgid[0] && !strcmp(id, rd->hr_q.hq_b.hp_msgid))
      ircd_strncpy(rd->hr_q.hq_b.hp_time, when,
                   sizeof(rd->hr_q.hq_b.hp_time) - 1);
  }

  /* A message the store has never heard of leaves its point with no time.
   * That is an empty answer and not an error: the client named something
   * this server does not have, which is what a split, a purge or a typo
   * all look like from here.
   */
  if (!rd->hr_q.hq_a.hp_time[0]
      || (rd->hr_q.hq_shape == HIST_BETWEEN && !rd->hr_q.hq_b.hp_time[0])) {
    if (rd->hr_cb)
      (rd->hr_cb)(1, 0, 0, rd->hr_user);
    MyFree(rd);
    return;
  }

  hist_read_run(rd);
}

int hist_store_read(const struct HistQuery* q, HistReadFn cb, void* user)
{
  struct HistRead* rd;
  struct DbParam p_a = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_b = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam* params[3];
  struct DbQuery query;
  enum DbError err;

  assert(0 != q);

  rd = (struct HistRead*) MyCalloc(1, sizeof(*rd));
  rd->hr_q = *q;
  rd->hr_cb = cb;
  rd->hr_user = user;

  /* A point named by message has to become a (time, message) pair before
   * anything can be compared against it.  Both points go in one lookup:
   * two round trips is the ceiling, not two per point.
   */
  if ((q->hq_a.hp_msgid[0] && !q->hq_a.hp_time[0])
      || (q->hq_b.hp_msgid[0] && !q->hq_b.hp_time[0])) {
    p_a.value = q->hq_a.hp_msgid[0] ? q->hq_a.hp_msgid : q->hq_b.hp_msgid;
    p_b.value = q->hq_b.hp_msgid[0] ? q->hq_b.hp_msgid : q->hq_a.hp_msgid;

    query.sql = "SELECT msgid, to_char(sent_at AT TIME ZONE 'UTC', "
                "'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS sent_at "
                "FROM message WHERE msgid = $1 OR msgid = $2";
    params[0] = &p_a;
    params[1] = &p_b;
    params[2] = NULL;
    query.params = params;

    err = db_query(hist_mod, &query, hist_resolve_done, rd);

    if (err != DB_OK) {
      hist_complain("resolve", 0, err);
      MyFree(rd);
      return 0;
    }

    return 1;
  }

  hist_read_run(rd);
  return 1;
}

/* ------------------------------------------------------------------- *
 * Exporting                                                           *
 * ------------------------------------------------------------------- */

/** Rows handed over at a time.
 *
 * Big enough that a long export is not a thousand round trips, small
 * enough that one page is a few hundred kilobytes rather than the whole
 * table in memory at once.
 */
#define HIST_EXPORT_PAGE 500

/** One export in flight. */
struct HistExport {
  char         hx_account[NICKLEN + 1];   /**< Whose. */
  HistExportFn hx_cb;                     /**< Who to hand the pages to. */
  void*        hx_user;                   /**< What to hand them with. */
  char         hx_time[40];               /**< Last row of the page before. */
  char         hx_msgid[MSGIDLEN + 1];    /**< Its identifier. */
  char         hx_limit[16];              /**< #HIST_EXPORT_PAGE, as text. */
};

/** The statement one page of an export runs.
 *
 * The keyset -- everything after the last row handed over -- rather than
 * an offset.  An offset re-reads and re-sorts everything before it on
 * every page, so exporting an account with a hundred thousand messages
 * would cost more the further it got; this is a range scan that starts
 * where the last one stopped.  The first page starts from the empty pair,
 * which sorts before every real row.
 */
static const char* hist_sql_export =
  "SELECT to_char(sent_at AT TIME ZONE 'UTC', "
  "'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS sent_at, msgid, kind, is_channel, "
  "target, coalesce(sender_prefix, sender_nick) AS prefix, "
  "coalesce(sender_account, '') AS from_account, "
  "coalesce(recipient_account, '') AS to_account, body, "
  "coalesce(reply_to, '') AS reply_to "
  "FROM message "
  "WHERE (sender_account = $1 OR recipient_account = $1) "
  "AND (sent_at, msgid) > ($2, $3) "
  "ORDER BY sent_at ASC, msgid ASC LIMIT $4";

static void hist_export_page(struct HistExport* hx);

/** Tell the caller the export stopped, and let it go. */
static void hist_export_fail(struct HistExport* hx)
{
  if (hx->hx_cb)
    (hx->hx_cb)(0, 0, 0, 1, hx->hx_user);
  MyFree(hx);
}

/** One page came back. */
static void hist_export_done(const struct DbResult* res, void* user)
{
  struct HistExport* hx = (struct HistExport*) user;
  struct HistExportRow* rows;
  struct HistCell* cells;
  char from[NICKLEN + 1];
  char to[NICKLEN + 1];
  unsigned int count;
  unsigned int i;
  int last;

  if (res->err.dberr_code != DB_OK) {
    hist_complain("export", res, DB_OK);
    hist_export_fail(hx);
    return;
  }

  count = db_rows(res->data);

  if (!count) {
    if (hx->hx_cb)
      (hx->hx_cb)(1, 0, 0, 1, hx->hx_user);
    MyFree(hx);
    return;
  }

  rows = (struct HistExportRow*) MyCalloc(count, sizeof(*rows));
  cells = (struct HistCell*) MyCalloc(count, sizeof(*cells));

  for (i = 0; i < count; i++) {
    ircd_strncpy(cells[i].hc_time, db_row_str(res->data, i, "sent_at"),
                 sizeof(cells[i].hc_time) - 1);
    ircd_strncpy(cells[i].hc_msgid, db_row_str(res->data, i, "msgid"),
                 sizeof(cells[i].hc_msgid) - 1);
    ircd_strncpy(cells[i].hc_target, db_row_str(res->data, i, "target"),
                 sizeof(cells[i].hc_target) - 1);
    ircd_strncpy(cells[i].hc_prefix, db_row_str(res->data, i, "prefix"),
                 sizeof(cells[i].hc_prefix) - 1);
    ircd_strncpy(cells[i].hc_body, db_row_str(res->data, i, "body"),
                 sizeof(cells[i].hc_body) - 1);
    ircd_strncpy(from, db_row_str(res->data, i, "from_account"),
                 sizeof(from) - 1);
    from[sizeof(from) - 1] = '\0';
    ircd_strncpy(to, db_row_str(res->data, i, "to_account"), sizeof(to) - 1);
    to[sizeof(to) - 1] = '\0';
    ircd_strncpy(cells[i].hc_from, from, sizeof(cells[i].hc_from) - 1);
    ircd_strncpy(cells[i].hc_to, to, sizeof(cells[i].hc_to) - 1);
    ircd_strncpy(cells[i].hc_reply, db_row_str(res->data, i, "reply_to"),
                 sizeof(cells[i].hc_reply) - 1);

    rows[i].he_time = cells[i].hc_time;
    rows[i].he_msgid = cells[i].hc_msgid;
    rows[i].he_kind = (int) db_row_int(res->data, i, "kind");
    rows[i].he_channel =
      !ircd_strcmp(db_row_str(res->data, i, "is_channel"), "true");
    rows[i].he_target = cells[i].hc_target;
    rows[i].he_prefix = cells[i].hc_prefix;
    rows[i].he_from = cells[i].hc_from;
    rows[i].he_to = cells[i].hc_to;
    rows[i].he_body = cells[i].hc_body;
    rows[i].he_reply = cells[i].hc_reply;
  }

  /* A short page is the last one: there was nothing else to fill it. */
  last = (count < HIST_EXPORT_PAGE);

  /* Where the next page starts, taken before the cells go. */
  ircd_strncpy(hx->hx_time, cells[count - 1].hc_time, sizeof(hx->hx_time) - 1);
  hx->hx_time[sizeof(hx->hx_time) - 1] = '\0';
  ircd_strncpy(hx->hx_msgid, cells[count - 1].hc_msgid,
               sizeof(hx->hx_msgid) - 1);
  hx->hx_msgid[sizeof(hx->hx_msgid) - 1] = '\0';

  if (hx->hx_cb)
    (hx->hx_cb)(1, rows, count, last, hx->hx_user);

  MyFree(cells);
  MyFree(rows);

  if (last)
    MyFree(hx);
  else
    hist_export_page(hx);
}

/** Ask for the page after the one already handed over. */
static void hist_export_page(struct HistExport* hx)
{
  struct DbParam p_account = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_time = { DB_TYPE_TIMESTAMPTZ, 0, DB_FORMAT_TEXT };
  struct DbParam p_msgid = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_limit = { DB_TYPE_INT, 0, DB_FORMAT_TEXT };
  struct DbParam* params[5];
  struct DbQuery query;
  enum DbError err;

  p_account.value = hx->hx_account;
  p_time.value = hx->hx_time;
  p_msgid.value = hx->hx_msgid;
  p_limit.value = hx->hx_limit;

  params[0] = &p_account;
  params[1] = &p_time;
  params[2] = &p_msgid;
  params[3] = &p_limit;
  params[4] = NULL;

  query.sql = hist_sql_export;
  query.params = params;

  err = db_query(hist_mod, &query, hist_export_done, hx);

  if (err != DB_OK) {
    hist_complain("export", 0, err);
    hist_export_fail(hx);
  }
}

int hist_store_export(const char* account, HistExportFn cb, void* user)
{
  struct HistExport* hx;

  assert(0 != account);

  hx = (struct HistExport*) MyCalloc(1, sizeof(*hx));
  ircd_strncpy(hx->hx_account, account, sizeof(hx->hx_account) - 1);
  hx->hx_cb = cb;
  hx->hx_user = user;

  /* The beginning of time, as a pair that sorts before every row. */
  ircd_strncpy(hx->hx_time, "0001-01-01T00:00:00.000Z",
               sizeof(hx->hx_time) - 1);
  hx->hx_msgid[0] = '\0';
  ircd_snprintf(0, hx->hx_limit, sizeof(hx->hx_limit), "%d",
                HIST_EXPORT_PAGE);

  hist_export_page(hx);

  return 1;
}

/* ------------------------------------------------------------------- *
 * Counting                                                            *
 * ------------------------------------------------------------------- */

/** What a caller of hist_store_count() is waiting on. */
struct HistCount {
  HistCountFn hc_cb;
  void*       hc_user;
};

/** What a count came back with. */
static void hist_count_done(const struct DbResult* res, void* user)
{
  struct HistCount* hc = (struct HistCount*) user;
  long long rows = -1;
  long long total = -1;

  if (res->err.dberr_code != DB_OK)
    hist_complain("count", res, DB_OK);
  else if (db_rows(res->data)) {
    rows = db_row_int(res->data, 0, "mine");
    total = db_row_int(res->data, 0, "total");
  }

  if (hc->hc_cb)
    (hc->hc_cb)(rows, total, hc->hc_user);

  MyFree(hc);
}

int hist_store_count(const char* account, HistCountFn cb, void* user)
{
  struct DbParam p_account = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam* params[2];
  struct DbQuery query;
  struct HistCount* hc;
  enum DbError err;

  /* One statement for both numbers: an operator asking how much of the
   * store is one person's wants to know what that is out of. */
  query.sql = "SELECT count(*) FILTER (WHERE sender_account = $1 "
              "OR recipient_account = $1) AS mine, count(*) AS total "
              "FROM message";

  p_account.value = account ? account : "";
  params[0] = &p_account;
  params[1] = NULL;
  query.params = params;

  hc = (struct HistCount*) MyCalloc(1, sizeof(*hc));
  hc->hc_cb = cb;
  hc->hc_user = user;

  err = db_query(hist_mod, &query, hist_count_done, hc);

  if (err != DB_OK) {
    hist_complain("count", 0, err);
    MyFree(hc);
    return 0;
  }

  return 1;
}

/* ------------------------------------------------------------------- *
 * Finding and redacting one message                                   *
 * ------------------------------------------------------------------- */

/** What a caller of hist_store_find() is waiting on. */
struct HistFind {
  HistFindFn hf_cb;
  void*      hf_user;
};

/** The lookup came back. */
static void hist_find_done(const struct DbResult* res, void* user)
{
  struct HistFind* hf = (struct HistFind*) user;
  struct HistFound found;

  memset(&found, 0, sizeof(found));

  if (res->err.dberr_code != DB_OK) {
    hist_complain("find", res, DB_OK);
    /* Not found and could-not-ask are told apart by the caller through
     * the NULL, because refusing a redaction because the database was
     * down is a different answer from refusing it because the message is
     * not there. */
    if (hf->hf_cb)
      (hf->hf_cb)(0, hf->hf_user);
    MyFree(hf);
    return;
  }

  if (!db_rows(res->data)) {
    if (hf->hf_cb)
      (hf->hf_cb)(0, hf->hf_user);
    MyFree(hf);
    return;
  }

  ircd_strncpy(found.hf_time, db_row_str(res->data, 0, "sent_at"),
               sizeof(found.hf_time) - 1);
  ircd_strncpy(found.hf_target, db_row_str(res->data, 0, "target"),
               sizeof(found.hf_target) - 1);
  ircd_strncpy(found.hf_canon, db_row_str(res->data, 0, "target_canon"),
               sizeof(found.hf_canon) - 1);
  ircd_strncpy(found.hf_account, db_row_str(res->data, 0, "account"),
               sizeof(found.hf_account) - 1);
  found.hf_channel =
    !ircd_strcmp(db_row_str(res->data, 0, "is_channel"), "true");

  if (hf->hf_cb)
    (hf->hf_cb)(&found, hf->hf_user);

  MyFree(hf);
}

int hist_store_find(const char* msgid, HistFindFn cb, void* user)
{
  struct DbParam p_msgid = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam* params[2];
  struct DbQuery query;
  struct HistFind* hf;
  enum DbError err;

  assert(0 != msgid);

  query.sql = "SELECT to_char(sent_at AT TIME ZONE 'UTC', "
              "'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS sent_at, "
              "target, target_canon, is_channel, "
              "coalesce(sender_account, '') AS account "
              "FROM message WHERE msgid = $1 LIMIT 1";

  p_msgid.value = msgid;
  params[0] = &p_msgid;
  params[1] = NULL;
  query.params = params;

  hf = (struct HistFind*) MyCalloc(1, sizeof(*hf));
  hf->hf_cb = cb;
  hf->hf_user = user;

  err = db_query(hist_mod, &query, hist_find_done, hf);

  if (err != DB_OK) {
    hist_complain("find", 0, err);
    MyFree(hf);
    return 0;
  }

  return 1;
}

/** What a redaction came back with. */
static void hist_redact_done(const struct DbResult* res, void* user)
{
  struct HistForget* hf = (struct HistForget*) user;
  long long rows = -1;

  if (res->err.dberr_code != DB_OK)
    hist_complain("redact", res, DB_OK);
  else if (db_rows(res->data))
    rows = db_row_int(res->data, 0, "history_redact");
  else
    rows = 0;

  if (hf->hf_cb)
    (hf->hf_cb)(rows, hf->hf_user);

  MyFree(hf);
}

int hist_store_redact(const char* msgid,
                      void (*cb)(long long rows, void* user), void* user)
{
  struct DbParam p_msgid = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam* params[2];
  struct DbQuery query;
  struct HistForget* hf;
  enum DbError err;

  assert(0 != msgid);

  /* The message and the reactions to it.  A reply is left alone: it is a
   * message of its own, somebody else said it, and redacting one message
   * is not permission to redact the conversation that followed. */
  query.sql = "SELECT history_redact($1)";

  p_msgid.value = msgid;
  params[0] = &p_msgid;
  params[1] = NULL;
  query.params = params;

  hf = (struct HistForget*) MyCalloc(1, sizeof(*hf));
  hf->hf_cb = cb;
  hf->hf_user = user;

  err = db_exec(hist_mod, &query, hist_redact_done, hf);

  if (err != DB_OK) {
    hist_complain("redact", 0, err);
    MyFree(hf);
    return 0;
  }

  return 1;
}

/* ------------------------------------------------------------------- *
 * Read markers                                                        *
 * ------------------------------------------------------------------- */

/** What a caller of the marker calls is waiting on. */
struct HistMarker {
  HistMarkerFn hm_cb;
  void*        hm_user;
};

/** A marker query came back. */
static void hist_marker_done(const struct DbResult* res, void* user)
{
  struct HistMarker* hm = (struct HistMarker*) user;

  if (res->err.dberr_code != DB_OK) {
    hist_complain("marker", res, DB_OK);
    if (hm->hm_cb)
      (hm->hm_cb)(0, "", hm->hm_user);
  } else if (hm->hm_cb) {
    /* No row is not a failure: it is somebody who has never marked this
     * conversation, which is most people and most conversations. */
    (hm->hm_cb)(1, db_rows(res->data) ? db_row_str(res->data, 0, "marker")
                                      : "", hm->hm_user);
  }

  MyFree(hm);
}

/** Shared body of the two marker calls. */
static int hist_marker_call(const struct DbQuery* query, int write,
                            HistMarkerFn cb, void* user)
{
  struct HistMarker* hm;
  enum DbError err;

  hm = (struct HistMarker*) MyCalloc(1, sizeof(*hm));
  hm->hm_cb = cb;
  hm->hm_user = user;

  err = write ? db_exec(hist_mod, query, hist_marker_done, hm)
              : db_query(hist_mod, query, hist_marker_done, hm);

  if (err != DB_OK) {
    hist_complain("marker", 0, err);
    MyFree(hm);
    return 0;
  }

  return 1;
}

int hist_store_marker_set(const char* account, const char* target,
                          const char* marker, HistMarkerFn cb, void* user)
{
  struct DbParam p_account = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_target = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_marker = { DB_TYPE_TIMESTAMPTZ, 0, DB_FORMAT_TEXT };
  struct DbParam* params[4];
  struct DbQuery query;

  assert(0 != account);
  assert(0 != target);
  assert(0 != marker);

  query.sql = "SELECT to_char(read_marker_set($1, $2, $3) AT TIME ZONE 'UTC',"
              " 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS marker";

  p_account.value = account;
  p_target.value = target;
  p_marker.value = marker;
  params[0] = &p_account;
  params[1] = &p_target;
  params[2] = &p_marker;
  params[3] = NULL;
  query.params = params;

  return hist_marker_call(&query, 1, cb, user);
}

int hist_store_marker_get(const char* account, const char* target,
                          HistMarkerFn cb, void* user)
{
  struct DbParam p_account = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam p_target = { DB_TYPE_TEXT, 0, DB_FORMAT_TEXT };
  struct DbParam* params[3];
  struct DbQuery query;

  assert(0 != account);
  assert(0 != target);

  query.sql = "SELECT to_char(marker AT TIME ZONE 'UTC', "
              "'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS marker "
              "FROM read_marker WHERE account = $1 AND target_canon = $2";

  p_account.value = account;
  p_target.value = target;
  params[0] = &p_account;
  params[1] = &p_target;
  params[2] = NULL;
  query.params = params;

  return hist_marker_call(&query, 0, cb, user);
}
