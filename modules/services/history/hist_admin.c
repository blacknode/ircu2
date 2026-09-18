/*
 * IRC - Internet Relay Chat, modules/services/history/hist_admin.c
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
 * @brief /HISTORY: what an operator does about stored messages.
 *
 * Four things, and they are all the same thing seen from different sides
 * -- what is kept, for how long, how it is handed over and how it is
 * destroyed:
 *
 *   HISTORY STATUS [<account>]  how much there is
 *   HISTORY PURGE               apply the retention now
 *   HISTORY EXPORT <account>    write out everything about one person
 *   HISTORY FORGET <account>    and delete it
 *
 * All four need @c history_admin in the Operator{} block, which is not a
 * default for anybody: it is the difference between an operator and
 * somebody who can read every conversation on the network, and that has
 * to be written down rather than arrived at by being an operator at all.
 *
 * EXPORT and FORGET read and delete by the same predicate -- every
 * message the account sent and every direct message it received -- and
 * that is deliberate.  What a person is handed has to be what a person
 * can have destroyed, or one of the two is lying about what "their data"
 * means.
 *
 * The export is a file rather than something sent over IRC.  What it is
 * for is a request that arrives by post and is answered by post; an
 * operator hands the file over.  It goes to HISTORY_EXPORT_DIR, which is
 * empty by default, so a server writes no files until it has been told
 * where to.
 */
#include "config.h"

#include "client.h"
#include "history.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "s_user.h"
#include "send.h"
#include "struct.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/** One export being written. */
struct HistDump {
  char   hd_numnick[10];        /**< The operator who asked. */
  time_t hd_born;               /**< When they connected. */
  char   hd_account[NICKLEN + 1]; /**< Whose history. */
  char   hd_path[512];          /**< Where it is going. */
  FILE*  hd_file;               /**< The file, while it is open. */
  long long hd_rows;            /**< Written so far. */
};

/** An export is a series of round trips, so there is at most one at a
 * time: two at once would interleave their pages through the same
 * callback and neither operator would get a whole file.
 */
static struct HistDump* hist_dump;

/** Say something to the operator who asked, if they are still there. */
static void hist_tell(const char* numnick, time_t born, const char* fmt, ...)
{
  struct Client* cptr = findNUser(numnick);
  char buf[BUFSIZE];
  va_list vl;

  if (!cptr || !MyUser(cptr) || cli_firsttime(cptr) != born)
    return;

  va_start(vl, fmt);
  ircd_vsnprintf(cptr, buf, sizeof(buf), _(cptr, fmt), vl);
  va_end(vl);

  sendcmdto_one(&me, CMD_NOTICE, cptr, "%C :%s", cptr, buf);
}

/* ------------------------------------------------------------------- *
 * STATUS                                                              *
 * ------------------------------------------------------------------- */

/** What the operator asked STATUS for. */
struct HistAsking {
  char   ha_numnick[10];
  time_t ha_born;
  char   ha_account[NICKLEN + 1];
};

/** The counts came back. */
static void hist_status_done(long long rows, long long total, void* user)
{
  struct HistAsking* ask = (struct HistAsking*) user;

  if (total < 0)
    hist_tell(ask->ha_numnick, ask->ha_born,
              N_("HISTORY: the store did not answer"));
  else if (ask->ha_account[0])
    hist_tell(ask->ha_numnick, ask->ha_born,
              N_("HISTORY: %s has %lld of %lld stored messages"),
              ask->ha_account, rows, total);
  else
    hist_tell(ask->ha_numnick, ask->ha_born,
              N_("HISTORY: %lld messages stored"), total);

  MyFree(ask);
}

/** HISTORY STATUS [<account>]. */
static void hist_status(struct Client* sptr, const char* account)
{
  struct HistAsking* ask;
  int days = feature_int(FEAT_HISTORY_RETENTION);

  if (days > 0)
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  "%C :HISTORY: keeping %d days; direct messages %s", sptr,
                  days, feature_bool(FEAT_HISTORY_PRIVATE) ? "on" : "off");
  else
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  "%C :HISTORY: keeping everything; direct messages %s", sptr,
                  feature_bool(FEAT_HISTORY_PRIVATE) ? "on" : "off");

  ask = (struct HistAsking*) MyCalloc(1, sizeof(*ask));
  ircd_snprintf(0, ask->ha_numnick, sizeof(ask->ha_numnick), "%s%s",
                NumNick(sptr));
  ask->ha_born = cli_firsttime(sptr);
  if (account)
    hist_canon(ask->ha_account, sizeof(ask->ha_account), account);

  if (!hist_store_count(ask->ha_account[0] ? ask->ha_account : 0,
                        hist_status_done, ask)) {
    send_fail(sptr, MSG_HISTORY, "MESSAGE_ERROR", "STATUS",
              _(sptr, N_("The store is not available right now")));
    MyFree(ask);
  }
}

/* ------------------------------------------------------------------- *
 * FORGET                                                              *
 * ------------------------------------------------------------------- */

/** The delete came back. */
static void hist_forget_done(long long rows, void* user)
{
  struct HistAsking* ask = (struct HistAsking*) user;

  if (rows < 0)
    hist_tell(ask->ha_numnick, ask->ha_born,
              N_("HISTORY: %s was not forgotten; the store did not answer"),
              ask->ha_account);
  else
    hist_tell(ask->ha_numnick, ask->ha_born,
              N_("HISTORY: forgot %lld messages belonging to %s"), rows,
              ask->ha_account);

  /* In the log too, and at NOTICE: this is somebody's record being
   * destroyed, and which operator did it is the part nobody can
   * reconstruct afterwards. */
  log_write(LS_SYSTEM, L_NOTICE, 0,
            "history: %lld messages belonging to %s were deleted", rows,
            ask->ha_account);

  MyFree(ask);
}

/** HISTORY FORGET <account>. */
static void hist_forget(struct Client* sptr, const char* account)
{
  struct HistAsking* ask;

  ask = (struct HistAsking*) MyCalloc(1, sizeof(*ask));
  ircd_snprintf(0, ask->ha_numnick, sizeof(ask->ha_numnick), "%s%s",
                NumNick(sptr));
  ask->ha_born = cli_firsttime(sptr);
  hist_canon(ask->ha_account, sizeof(ask->ha_account), account);

  if (!hist_store_forget(ask->ha_account, hist_forget_done, ask)) {
    send_fail(sptr, MSG_HISTORY, "MESSAGE_ERROR", "FORGET",
              _(sptr, N_("The store is not available right now")));
    MyFree(ask);
  }
}

/* ------------------------------------------------------------------- *
 * EXPORT                                                              *
 * ------------------------------------------------------------------- */

/** Write \a text as a JSON string, escaped, to \a out. */
static void hist_json_string(FILE* out, const char* text)
{
  const unsigned char* p;

  fputc('"', out);

  for (p = (const unsigned char*) (text ? text : ""); *p; p++) {
    switch (*p) {
    case '"':  fputs("\\\"", out); break;
    case '\\': fputs("\\\\", out); break;
    case '\n': fputs("\\n", out); break;
    case '\r': fputs("\\r", out); break;
    case '\t': fputs("\\t", out); break;
    default:
      /* Everything below a space, and the control codes IRC uses for
       * colour and formatting, have to be escaped or the file is not
       * JSON.  Above that the bytes are passed through: the server never
       * decided what encoding a message was in and inventing one here
       * would change what was said. */
      if (*p < 0x20)
        fprintf(out, "\\u%04x", *p);
      else
        fputc(*p, out);
      break;
    }
  }

  fputc('"', out);
}

/** Write one message as a line of JSON. */
static void hist_json_row(FILE* out, const struct HistExportRow* row)
{
  fputs("{\"time\":", out);
  hist_json_string(out, row->he_time);
  fputs(",\"msgid\":", out);
  hist_json_string(out, row->he_msgid);
  fprintf(out, ",\"kind\":%d,\"channel\":%s,\"target\":",
          row->he_kind, row->he_channel ? "true" : "false");
  hist_json_string(out, row->he_target);
  fputs(",\"from\":", out);
  hist_json_string(out, row->he_prefix);
  fputs(",\"from_account\":", out);
  hist_json_string(out, row->he_from);
  fputs(",\"to_account\":", out);
  hist_json_string(out, row->he_to);
  fputs(",\"text\":", out);
  hist_json_string(out, row->he_body);
  fputs("}\n", out);
}

/** Close the file and let the export go. */
static void hist_dump_finish(int ok)
{
  struct HistDump* hd = hist_dump;

  if (!hd)
    return;

  if (hd->hd_file) {
    fclose(hd->hd_file);
    hd->hd_file = NULL;
  }

  if (ok)
    hist_tell(hd->hd_numnick, hd->hd_born,
              N_("HISTORY: exported %lld messages belonging to %s to %s"),
              hd->hd_rows, hd->hd_account, hd->hd_path);
  else
    hist_tell(hd->hd_numnick, hd->hd_born,
              N_("HISTORY: the export of %s stopped after %lld messages; "
                 "%s is incomplete"), hd->hd_account, hd->hd_rows,
              hd->hd_path);

  log_write(LS_SYSTEM, ok ? L_INFO : L_ERROR, 0,
            "history: export of %s to %s: %lld messages%s", hd->hd_account,
            hd->hd_path, hd->hd_rows, ok ? "" : " (incomplete)");

  MyFree(hd);
  hist_dump = NULL;
}

/** One page of the export arrived. */
static void hist_dump_page(int ok, const struct HistExportRow* rows,
                           unsigned int count, int done, void* user)
{
  struct HistDump* hd = (struct HistDump*) user;
  unsigned int i;

  /* Not ours any more: the operator left, the module was reloaded.  The
   * pages keep arriving because db.c owes this module an answer, and
   * dropping them is all there is to do. */
  if (hd != hist_dump)
    return;

  if (!ok) {
    hist_dump_finish(0);
    return;
  }

  for (i = 0; i < count; i++) {
    hist_json_row(hd->hd_file, &rows[i]);
    hd->hd_rows++;
  }

  if (ferror(hd->hd_file)) {
    log_write(LS_SYSTEM, L_ERROR, 0, "history: writing %s: %s", hd->hd_path,
              strerror(errno));
    hist_dump_finish(0);
    return;
  }

  if (done)
    hist_dump_finish(1);
}

/** HISTORY EXPORT <account>. */
static void hist_export(struct Client* sptr, const char* account)
{
  const char* dir = feature_str(FEAT_HISTORY_EXPORT_DIR);
  struct HistDump* hd;
  struct tm tm;
  time_t now = CurrentTime;
  FILE* out;
  char canon[NICKLEN + 1];
  char path[512];

  if (EmptyString(dir)) {
    send_fail(sptr, MSG_HISTORY, "INVALID_PARAMS", "EXPORT",
              _(sptr, N_("HISTORY_EXPORT_DIR is not set, so there is "
                         "nowhere to write an export")));
    return;
  }

  if (hist_dump) {
    send_fail(sptr, MSG_HISTORY, "MESSAGE_ERROR", "EXPORT",
              _(sptr, N_("Another export is running; wait for it to "
                         "finish")));
    return;
  }

  hist_canon(canon, sizeof(canon), account);

  gmtime_r(&now, &tm);
  ircd_snprintf(0, path, sizeof(path),
                "%s/%s-%04d%02d%02dT%02d%02d%02dZ.jsonl", dir, canon,
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec);

  /* "x": never over an existing file.  The name has a timestamp in it, so
   * a collision means something else is writing there, and an export that
   * quietly replaced somebody else's would be the worst possible failure
   * for this particular file. */
  out = fopen(path, "wx");

  if (!out) {
    log_write(LS_SYSTEM, L_ERROR, 0, "history: cannot write %s: %s", path,
              strerror(errno));
    send_fail(sptr, MSG_HISTORY, "MESSAGE_ERROR", "EXPORT",
              _(sptr, N_("The export file could not be created; see the "
                         "server log")));
    return;
  }

  /* The file is one person's whole record.  Nobody but the account the
   * server runs as has any business reading it off the disk. */
  fchmod(fileno(out), S_IRUSR | S_IWUSR);

  hd = (struct HistDump*) MyCalloc(1, sizeof(*hd));
  ircd_snprintf(0, hd->hd_numnick, sizeof(hd->hd_numnick), "%s%s",
                NumNick(sptr));
  hd->hd_born = cli_firsttime(sptr);
  ircd_strncpy(hd->hd_account, canon, sizeof(hd->hd_account) - 1);
  ircd_strncpy(hd->hd_path, path, sizeof(hd->hd_path) - 1);
  hd->hd_file = out;

  hist_dump = hd;

  sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :HISTORY: exporting %s to %s",
                sptr, canon, path);

  if (!hist_store_export(canon, hist_dump_page, hd))
    hist_dump_finish(0);
}

/** Give up on an export because the module is going away. */
void hist_admin_shutdown(void)
{
  if (hist_dump)
    hist_dump_finish(0);
}

/* ------------------------------------------------------------------- *
 * The command                                                         *
 * ------------------------------------------------------------------- */

int hist_m_history(struct Client* cptr, struct Client* sptr, int parc,
                   char* parv[])
{
  if (!HasPriv(sptr, PRIV_HISTORY))
    return send_reply(sptr, ERR_NOPRIVILEGES);

  if (parc < 2 || EmptyString(parv[1]))
    return need_more_params(sptr, MSG_HISTORY);

  if (!ircd_strcmp(parv[1], "STATUS")) {
    hist_status(sptr, (parc > 2 && !EmptyString(parv[2])) ? parv[2] : 0);
    return 0;
  }

  if (!ircd_strcmp(parv[1], "PURGE")) {
    int days = feature_int(FEAT_HISTORY_RETENTION);

    if (days <= 0) {
      send_fail(sptr, MSG_HISTORY, "INVALID_PARAMS", "PURGE",
                _(sptr, N_("HISTORY_RETENTION is 0, which means keep "
                           "everything; there is nothing to purge")));
      return 0;
    }

    hist_store_purge(days);
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  "%C :HISTORY: purging everything older than %d days",
                  sptr, days);
    return 0;
  }

  if (!ircd_strcmp(parv[1], "EXPORT") || !ircd_strcmp(parv[1], "FORGET")) {
    if (parc < 3 || EmptyString(parv[2]))
      return need_more_params(sptr, MSG_HISTORY);

    if (!ircd_strcmp(parv[1], "EXPORT"))
      hist_export(sptr, parv[2]);
    else
      hist_forget(sptr, parv[2]);

    return 0;
  }

  send_fail(sptr, MSG_HISTORY, "UNKNOWN_COMMAND", parv[1],
            _(sptr, N_("HISTORY takes STATUS, PURGE, EXPORT or FORGET")));

  return 0;
}
