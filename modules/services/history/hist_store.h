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
#ifndef INCLUDED_ircd_defs_h
#include "ircd_defs.h"
#endif
#ifndef INCLUDED_msgid_h
#include "msgid.h"
#endif
#ifndef INCLUDED_channel_h
#include "channel.h"
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
  const char*   hm_prefix;     /**< nick!user@host as they were. */
  const char*   hm_body;       /**< The text, or a reaction for a TAGMSG. */
  const char*   hm_reply;      /**< Message replied to or reacted to. */
};

/** Which way a request looks at the store.
 *
 * The six shapes CHATHISTORY has, named here rather than spelled out in
 * SQL wherever somebody needed one.
 */
enum HistShape {
  HIST_LATEST,   /**< The newest messages there are. */
  HIST_BEFORE,   /**< The newest messages older than a point. */
  HIST_AFTER,    /**< The oldest messages newer than a point. */
  HIST_AROUND,   /**< Half either side of a point. */
  HIST_BETWEEN,  /**< What lies between two points. */
  HIST_TARGETS   /**< Who this client has conversations with. */
};

/** A point in the conversation.
 *
 * A client names one by time or by message.  Both are filled in before a
 * query is built -- a bare timestamp gets an empty identifier, which
 * sorts before every real one -- so that the comparison is always on the
 * pair and paging can never repeat or skip a message that shares a second
 * with another.
 */
struct HistPoint {
  char hp_time[32];             /**< ISO 8601, or "" for no point. */
  char hp_msgid[MSGIDLEN + 1];  /**< The message, or "". */
};

/** What a read is asking for.
 *
 * Who is asking is already resolved: #hq_self and #hq_peer are accounts,
 * canonical, and whether the asker may see any of this was decided before
 * this struct was filled in.  The store does not do permissions -- it
 * would be the second place they were decided.
 */
struct HistQuery {
  enum HistShape hq_shape;              /**< Which of the six. */
  int            hq_channel;            /**< Target is a channel. */
  char           hq_canon[CHANNELLEN + 1]; /**< The channel, canonical. */
  char           hq_self[ACCOUNTLEN + 1]; /**< The asker's account. */
  char           hq_peer[ACCOUNTLEN + 1]; /**< The other end's account. */
  struct HistPoint hq_a;                /**< The point, or the first. */
  struct HistPoint hq_b;                /**< The second, for BETWEEN. */
  unsigned int   hq_limit;              /**< Most rows to return. */
};

/** One row on its way back out.
 *
 * Every string is valid only for the length of the callback.  For
 * #HIST_TARGETS only #hr_target and #hr_time are filled in: the target,
 * and when it was last spoken to.
 */
struct HistRow {
  const char*   hr_time;     /**< When, ISO 8601. */
  const char*   hr_msgid;    /**< Its name, or "" if it has none. */
  enum HistKind hr_kind;     /**< PRIVMSG or NOTICE. */
  const char*   hr_target;   /**< Channel or nickname, as addressed. */
  const char*   hr_prefix;   /**< nick!user@host, or the bare nickname. */
  const char*   hr_body;     /**< The text, or a reaction for a TAGMSG. */
  const char*   hr_reply;    /**< Message replied to or reacted to, or "". */
  const char*   hr_edited;   /**< When it was last changed, or "". */
};

/** Receives the answer to hist_store_read().  Runs in the main thread.
 *
 * @param[in] ok Non-zero if the store answered at all.
 * @param[in] rows Oldest first, always: a transcript arrives in the order
 *   it happened whichever end the client asked from.
 * @param[in] count How many.
 * @param[in] user What was handed to hist_store_read().
 */
typedef void (*HistReadFn)(int ok, const struct HistRow* rows,
                           unsigned int count, void* user);

/** Read from the store.
 *
 * Two round trips at most: a request that names a point by message has
 * that resolved to its (time, message) pair first, so everything after it
 * compares pairs and the six shapes are one statement each.
 *
 * The two points of a #HIST_BETWEEN may be given in either order: which
 * one is earlier is decided by the database, which is the thing that
 * compares them.
 *
 * @param[in] q What to look for.  Copied before this returns.
 * @param[in] cb Called in the main thread with the rows.
 * @param[in] user Passed through.
 * @return Non-zero if the request was accepted and \a cb will run.
 */
extern int hist_store_read(const struct HistQuery* q, HistReadFn cb,
                           void* user);

/** One page of an export, on its way out.
 *
 * Everything about a message, including the two accounts, because what is
 * being handed over is the record rather than a transcript to read.
 */
struct HistExportRow {
  const char* he_time;       /**< When, ISO 8601. */
  const char* he_msgid;      /**< Its name. */
  int         he_kind;       /**< 0 PRIVMSG, 1 NOTICE, 2 TAGMSG. */
  int         he_channel;    /**< Whether the target is a channel. */
  const char* he_target;     /**< Channel or nickname, as addressed. */
  const char* he_prefix;     /**< nick!user@host at the time. */
  const char* he_from;       /**< Sender's account, or "". */
  const char* he_to;         /**< Recipient's account, or "". */
  const char* he_body;       /**< The text, as it stands now. */
  const char* he_reply;      /**< Message replied to or reacted to, or "". */
  const char* he_edited;     /**< When it was last changed, or "". */
};

/** Receives one page of an export.  Runs in the main thread.
 *
 * Called once per page, oldest first, and once more with \a count zero
 * when there are no more; \a ok is zero if the store stopped answering,
 * in which case the export is incomplete and its caller has to say so.
 *
 * @param[in] ok Non-zero if the page was read.
 * @param[in] rows The messages.
 * @param[in] count How many, zero at the end.
 * @param[in] done Non-zero on the last call.
 * @param[in] user What was handed to hist_store_export().
 */
typedef void (*HistExportFn)(int ok, const struct HistExportRow* rows,
                             unsigned int count, int done, void* user);

/** Read everything one account said or was told.
 *
 * The same predicate hist_store_forget() deletes by, which is the point:
 * what a person is given has to be what a person can have deleted, or one
 * of the two is lying.
 *
 * Paged with a keyset rather than an offset -- @c (sent_at, @c msgid)
 * greater than the last row of the page before -- so a long export is a
 * series of index range scans and never re-reads what it has already
 * handed over.  There is no cursor to hold open: db.h has no
 * transactions, and a pooled connection is not the caller's to keep.
 *
 * @param[in] account The account, canonical.
 * @param[in] cb Called once per page and once at the end.
 * @param[in] user Passed through.
 * @return Non-zero if the export started.
 */
extern int hist_store_export(const char* account, HistExportFn cb,
                             void* user);

/** How much of the store belongs to one account, and how big it all is.
 *
 * @param[in] rows Messages the account sent or received, -1 on failure.
 * @param[in] total Messages stored altogether.
 * @param[in] user What was handed to hist_store_count().
 */
typedef void (*HistCountFn)(long long rows, long long total, void* user);

/** Count what is stored.
 * @param[in] account The account to count, canonical, or NULL for none.
 * @param[in] cb Called in the main thread with the answer.
 * @param[in] user Passed through.
 * @return Non-zero if the request was accepted.
 */
extern int hist_store_count(const char* account, HistCountFn cb, void* user);

/** What a stored message is, for the purpose of deciding about it.
 *
 * Enough to answer "may this person redact this?" and nothing else: who
 * wrote it, where, and when.
 */
struct HistFound {
  char hf_time[40];                /**< When, ISO 8601, or "" if not found. */
  char hf_target[CHANNELLEN + 1];  /**< Channel or nickname, as addressed. */
  char hf_canon[CHANNELLEN + 1];   /**< Its canonical form. */
  char hf_account[ACCOUNTLEN + 1]; /**< Who wrote it, or "". */
  int  hf_channel;                 /**< Whether the target is a channel. */
  enum HistKind hf_kind;           /**< What it arrived as. */
};

/** Receives the answer to hist_store_find().  Runs in the main thread.
 * @param[in] found The row, or NULL when there is no such message.
 * @param[in] user What was handed to hist_store_find().
 */
typedef void (*HistFindFn)(const struct HistFound* found, void* user);

/** Find one message by the name the network knows it by.
 * @param[in] msgid The identifier.
 * @param[in] cb Called in the main thread with the answer.
 * @param[in] user Passed through.
 * @return Non-zero if the request was accepted.
 */
extern int hist_store_find(const char* msgid, HistFindFn cb, void* user);

/** Delete one message and everything that reacted to it.
 *
 * A reaction to a message that is gone is a reference to nothing, so the
 * reactions go with it -- but a *reply* does not: a reply is a message of
 * its own, somebody else said it, and redacting one message is not
 * permission to redact the conversation that followed.
 *
 * @param[in] msgid The identifier.
 * @param[in] cb Called with how many rows went, or -1; may be NULL.
 * @param[in] user Passed through.
 * @return Non-zero if the request was accepted.
 */
extern int hist_store_redact(const char* msgid,
                             void (*cb)(long long rows, void* user),
                             void* user);

/** Receives a read marker.  Runs in the main thread.
 * @param[in] ok Non-zero if the store answered.
 * @param[in] marker Where the marker is now, ISO 8601, or "" for none.
 * @param[in] user What was handed to the call.
 */
typedef void (*HistMarkerFn)(int ok, const char* marker, void* user);

/** Move a read marker forward, and say where it ended up.
 *
 * Only ever forward.  Two clients of the same person race constantly --
 * one is catching up while the other is at the bottom -- and a marker
 * that could move backwards would make messages unread again every time
 * the slower one reported in.  The answer is where the marker *is*,
 * which may not be where the caller asked to put it, and that is what
 * the client is told.
 *
 * @param[in] account The account, canonical.
 * @param[in] target The conversation, canonical.
 * @param[in] marker Where to move it to, ISO 8601.
 * @param[in] cb Called in the main thread.
 * @param[in] user Passed through.
 * @return Non-zero if the request was accepted.
 */
extern int hist_store_marker_set(const char* account, const char* target,
                                 const char* marker, HistMarkerFn cb,
                                 void* user);

/** Read a marker back.
 * @param[in] account The account, canonical.
 * @param[in] target The conversation, canonical.
 * @param[in] cb Called in the main thread; "" when there is no marker.
 * @param[in] user Passed through.
 * @return Non-zero if the request was accepted.
 */
extern int hist_store_marker_get(const char* account, const char* target,
                                 HistMarkerFn cb, void* user);

/** What a search is looking for.
 *
 * Who is asking has already been resolved and what they may see has
 * already been decided: #hs_canon is the list of channels this client is
 * on, #hs_self its account, and neither is checked again here.  The store
 * does not do permissions -- it would be the second place they were
 * decided.
 */
struct HistSearch {
  char hs_self[ACCOUNTLEN + 1]; /**< The asker's account, canonical. */
  char hs_peer[ACCOUNTLEN + 1]; /**< One conversation, or "". */
  char hs_from[ACCOUNTLEN + 1]; /**< Only from this account, or "". */
  const char* hs_channels;      /**< Channels, as a PostgreSQL array
                                     literal, or NULL for none. */
  char hs_text[BUFSIZE];        /**< What to look for, as a person wrote it. */
  char hs_after[40];            /**< Nothing before this, or "". */
  char hs_before[40];           /**< Nothing after this, or "". */
  unsigned int hs_limit;        /**< Most rows to return. */
};

/** Search the store.
 *
 * One GIN lookup over @c body_search and then a filter, which is why the
 * text is the first thing in the WHERE clause: a query that narrowed by
 * channel first would read every message that channel ever had.
 *
 * The rows come back newest first -- what a person searching wants is
 * what was said most recently -- and are handed to \a cb oldest first,
 * the way every other read is.
 *
 * @param[in] s What to look for.  Copied before this returns.
 * @param[in] cb Called in the main thread with the rows.
 * @param[in] user Passed through.
 * @return Non-zero if the request was accepted and \a cb will run.
 */
extern int hist_store_search(const struct HistSearch* s, HistReadFn cb,
                             void* user);

/** Change what one message says.
 *
 * The identifier does not change, which is the whole reason an edit is
 * not simply another message: the replies and the reactions point at it,
 * and a message that was edited is still the message they are about.
 *
 * The timestamp is taken as well as the identifier because the table is
 * partitioned by it: an UPDATE that named only the identifier would visit
 * every partition there is.
 *
 * @param[in] msgid The identifier.
 * @param[in] sent_at When it was sent, ISO 8601, from hist_store_find().
 * @param[in] body What it says now.
 * @param[in] cb Called with how many rows changed, or -1; may be NULL.
 * @param[in] user Passed through.
 * @return Non-zero if the request was accepted.
 */
extern int hist_store_edit(const char* msgid, const char* sent_at,
                           const char* body,
                           void (*cb)(long long rows, void* user),
                           void* user);

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

/** Non-zero once a statement of this module's has reached the schema.
 *
 * The schema is applied by an operator, which may well be minutes or days
 * after the module was loaded, so "is it there yet" is a question with a
 * changing answer rather than a configuration setting.
 */
extern int hist_store_ready(void);

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
