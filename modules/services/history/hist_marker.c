/*
 * IRC - Internet Relay Chat, modules/services/history/hist_marker.c
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
 * @brief MARKREAD: how far somebody has read.
 *
 * IRCv3 @c draft/read-marker.  It is the thing that turns a history into
 * an inbox: without it a client reconnecting can fetch everything it
 * missed but cannot tell which of it it has already seen, so either
 * everything is unread or nothing is.
 *
 *   MARKREAD <target>                       what is my marker?
 *   MARKREAD <target> timestamp=<ISO 8601>  move it here
 *   MARKREAD <target> *                     I have read nothing
 *
 * and the answer to all three is the same line, saying where the marker
 * *is*:
 *
 *   MARKREAD <target> timestamp=<ISO 8601>
 *   MARKREAD <target> *
 *
 * Two things are worth knowing about it.
 *
 * **It is per account, not per connection.**  A person reads on their
 * phone and expects their laptop to know, which is the whole point; a
 * client with no account has nowhere to keep this and is told so.
 *
 * **It only moves forward.**  Two clients of the same person race
 * constantly -- one is catching up while the other is at the bottom --
 * and a marker that could move backwards would make messages unread again
 * every time the slower one reported in.  So the answer is where the
 * marker ended up, which may not be where the client asked to put it, and
 * every one of that person's clients is told.
 */
#include "config.h"

#include "channel.h"
#include "client.h"
#include "hash.h"
#include "history.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_i18n.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "s_bsd.h"
#include "s_user.h"
#include "send.h"
#include "struct.h"

#include <string.h>

/** One MARKREAD waiting on the store. */
struct HistMark {
  char hm_numnick[10];             /**< Who asked. */
  time_t hm_born;                  /**< When they connected. */
  char hm_account[NICKLEN + 1];    /**< Their account, canonical. */
  char hm_target[CHANNELLEN + 1];  /**< As they wrote it. */
  char hm_canon[CHANNELLEN + 1];   /**< Canonical. */
  int  hm_broadcast;               /**< Tell their other clients too. */
};

/** Send one MARKREAD line.
 * @param[in] to Client to tell.
 * @param[in] target The conversation, as the client writes it.
 * @param[in] marker Where the marker is, or "" for nowhere.
 */
static void hist_mark_tell(struct Client* to, const char* target,
                           const char* marker)
{
  if (!CapActive(to, hist_marker_cap))
    return;

  if (marker && *marker)
    sendcmdto_one(&me, MSG_MARKREAD, TOK_MARKREAD, to, "%s timestamp=%s",
                  target, marker);
  else
    sendcmdto_one(&me, MSG_MARKREAD, TOK_MARKREAD, to, "%s *", target);
}

/** Tell every connection of one account where the marker is now.
 *
 * The marker belongs to the person, so all of their clients are told, not
 * only the one that moved it.  A phone that marks a conversation read is
 * the usual way a laptop finds out.
 *
 * @param[in] account The account, canonical.
 * @param[in] target The conversation, as written.
 * @param[in] marker Where it is.
 */
static void hist_mark_broadcast(const char* account, const char* target,
                                const char* marker)
{
  int i;

  for (i = 0; i <= HighestFd; i++) {
    struct Client* cptr = LocalClientArray[i];
    char mine[NICKLEN + 1];

    if (!cptr || !IsUser(cptr) || !IsAccount(cptr) || !cli_user(cptr))
      continue;
    if (!cli_user(cptr)->account[0])
      continue;

    hist_canon(mine, sizeof(mine), cli_user(cptr)->account);

    if (ircd_strcmp(mine, account))
      continue;

    hist_mark_tell(cptr, target, marker);
  }
}

/** The store answered. */
static void hist_mark_done(int ok, const char* marker, void* user)
{
  struct HistMark* mk = (struct HistMark*) user;
  struct Client* cptr = findNUser(mk->hm_numnick);

  if (cptr && (!MyUser(cptr) || cli_firsttime(cptr) != mk->hm_born))
    cptr = 0;

  if (!ok) {
    if (cptr)
      send_fail(cptr, MSG_MARKREAD, "INTERNAL_ERROR", mk->hm_target,
                _(cptr, N_("The read marker could not be read")));
    MyFree(mk);
    return;
  }

  if (mk->hm_broadcast)
    hist_mark_broadcast(mk->hm_account, mk->hm_target, marker);
  else if (cptr)
    hist_mark_tell(cptr, mk->hm_target, marker);

  MyFree(mk);
}

/** MARKREAD from a client.
 * @param[in] cptr Connection it arrived on.
 * @param[in] sptr Who sent it.
 * @param[in] parc Parameter count.
 * @param[in] parv Parameters.
 * @return Zero.
 */
int hist_m_markread(struct Client* cptr, struct Client* sptr, int parc,
                    char* parv[])
{
  struct HistMark* mk;
  const char* want = 0;
  int ok;

  if (parc < 2 || EmptyString(parv[1]))
    return need_more_params(sptr, MSG_MARKREAD);

  /* Per account, not per connection.  A person reads on their phone and
   * expects their laptop to know, and a client with no account has no
   * person for this to belong to. */
  if (!IsAccount(sptr) || !cli_user(sptr)->account[0]) {
    send_fail(sptr, MSG_MARKREAD, "NEED_REGISTRATION", parv[1],
              _(sptr, N_("You have to be identified to keep read markers")));
    return 0;
  }

  if (parc > 2 && !EmptyString(parv[2]) && strcmp(parv[2], "*")) {
    if (ircd_strncmp(parv[2], "timestamp=", 10)) {
      send_fail(sptr, MSG_MARKREAD, "INVALID_PARAMS", parv[2],
                _(sptr, N_("A marker is timestamp=<time> or *")));
      return 0;
    }

    want = parv[2] + 10;

    if (!*want) {
      send_fail(sptr, MSG_MARKREAD, "INVALID_PARAMS", parv[2],
                _(sptr, N_("A marker is timestamp=<time> or *")));
      return 0;
    }
  }

  mk = (struct HistMark*) MyCalloc(1, sizeof(*mk));
  ircd_snprintf(0, mk->hm_numnick, sizeof(mk->hm_numnick), "%s%s",
                NumNick(sptr));
  mk->hm_born = cli_firsttime(sptr);
  hist_canon(mk->hm_account, sizeof(mk->hm_account),
             cli_user(sptr)->account);
  ircd_strncpy(mk->hm_target, parv[1], sizeof(mk->hm_target) - 1);
  hist_canon(mk->hm_canon, sizeof(mk->hm_canon), parv[1]);

  if (want) {
    /* A move is told to every one of that person's clients; a question is
     * answered to the one that asked. */
    mk->hm_broadcast = 1;
    ok = hist_store_marker_set(mk->hm_account, mk->hm_canon, want,
                               hist_mark_done, mk);
  } else {
    ok = hist_store_marker_get(mk->hm_account, mk->hm_canon,
                               hist_mark_done, mk);
  }

  if (!ok) {
    send_fail(sptr, MSG_MARKREAD, "INTERNAL_ERROR", mk->hm_target,
              _(sptr, N_("The store is not available right now")));
    MyFree(mk);
  }

  return 0;
}
