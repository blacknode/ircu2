/*
 * IRC - Internet Relay Chat, modules/services/history/hist_store.h
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
 * @brief The store, as the rest of the module sees it.
 *
 * Every statement this module sends is written here and nowhere else.
 * That is not a door left open for a second database engine -- proposal
 * 006 section 7.1 closes that question -- it is that the code which answers
 * a client has no business knowing SQL, and that retention, purging and
 * deletion by account need one place where they happen rather than a
 * statement in whichever file first needed one.
 *
 * Nothing here blocks.  Every call goes out through db.h, which hands the
 * work to the driver's own threads and calls back in the main thread.
 */
#ifndef INCLUDED_hist_store_h
#define INCLUDED_hist_store_h

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

struct ModuleHandle;

/** Which command a stored message arrived as.
 *
 * The values are written to the database, so they are fixed: a migration
 * would be needed to change one, which is the point.
 */
enum HistKind {
  HIST_PRIVMSG = 0,  /**< PRIVMSG. */
  HIST_NOTICE  = 1,  /**< NOTICE. */
  HIST_TAGMSG  = 2   /**< TAGMSG: no text of its own. */
};

/** One message, on its way in.
 *
 * Every string is NUL-terminated and belongs to the caller; the store
 * copies what it needs before returning.  A NULL is SQL NULL.
 */
struct HistMessage {
  const char*   hm_msgid;      /**< What the network calls this message. */
  const char*   hm_time;       /**< When, ISO 8601 with milliseconds. */
  enum HistKind hm_kind;       /**< PRIVMSG, NOTICE or TAGMSG. */
  int           hm_channel;    /**< Non-zero if the target is a channel. */
  const char*   hm_target;     /**< Channel or nickname, as addressed. */
  const char*   hm_canon;      /**< Its canonical form. */
  const char*   hm_sender;     /**< The sender's nickname. */
  const char*   hm_account;    /**< What the sender had proved, or NULL. */
  const char*   hm_recipient;  /**< What the recipient had proved, for a
                                    direct message; NULL for a channel. */
  const char*   hm_body;       /**< The text; "" for a TAGMSG. */
};

/** Store one message.
 *
 * Fire and forget: there is nobody to tell if it fails, and a message
 * whose insert was refused is not a reason to refuse the message itself,
 * which has already gone out.  Failures are logged, rate-limited, because
 * a database that is down would otherwise fill the log at the speed of the
 * network's traffic.
 *
 * Every server that delivered the message calls this, so the insert is
 * written to do nothing when the row is already there.
 *
 * @param[in] msg What to store.
 * @return Non-zero if the write was accepted for sending.
 */
extern int hist_store_write(const struct HistMessage* msg);

/** Make sure the partitions around \a when exist.
 *
 * Called at load and once a day thereafter, for this month and the next,
 * so that the month boundary is not the first time anybody finds out.
 * @param[in] ahead How many months past the current one to create.
 */
extern void hist_store_ensure_partitions(int ahead);

/** Drop everything older than \a days days.
 *
 * Does nothing when \a days is zero, which is what "keep everything"
 * means.
 */
extern void hist_store_purge(int days);

/** Forget everything one account said or was told.
 *
 * A direct message is one row, so this takes the other end's copy of the
 * conversation with it.  That is what is being asked for: the message is
 * what is deleted, and there is only one of it.
 *
 * @param[in] account The account, canonical.
 * @param[in] cb Called with how many rows went, or -1 on failure; may be
 *   NULL.
 * @param[in] user Passed through.
 * @return Non-zero if the request was accepted.
 */
extern int hist_store_forget(const char* account,
                             void (*cb)(long long rows, void* user),
                             void* user);

#endif /* INCLUDED_hist_store_h */
