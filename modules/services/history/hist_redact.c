/*
 * IRC - Internet Relay Chat, modules/services/history/hist_redact.c
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
 * @brief REDACT: taking a message back.
 *
 * IRCv3 @c draft/message-redaction, and it lives in this module rather
 * than in the core for one reason: **you cannot redact what nobody
 * stored**.  A server with no history has nothing to delete and no way to
 * know who wrote the message being pointed at, so the command would be a
 * announcement that something was withdrawn with nothing withdrawn.
 *
 * That is also what makes the policy checkable.  Whether somebody may
 * take a message back depends on who wrote it, and the only thing that
 * knows is the row -- so the answer is a database round trip away, and
 * the command waits for it before it tells anybody anything.  Nothing is
 * relayed until the deletion has happened: a client that removed the
 * message from its view while the store kept it would be the one outcome
 * worse than not supporting this at all.
 *
 * Who may:
 *
 *   - the person who wrote it, within @c HISTORY_REDACT_WINDOW seconds
 *     (0 means for ever);
 *   - anybody with ops on the channel it was said in, with no window,
 *     because moderating is not undoing;
 *   - an operator with @c history_admin, for the same reason they can
 *     delete an account's whole record.
 *
 * A direct message is the first case only.  There is nobody with ops on a
 * conversation.
 */
#include "config.h"

#include "channel.h"
#include "client.h"
#include "hash.h"
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
#include "msgid.h"
#include "numeric.h"
#include "numnicks.h"
#include "s_user.h"
#include "send.h"
#include "struct.h"

#include <string.h>

/** One redaction waiting on the store. */
struct HistRedact {
  char hr_numnick[10];             /**< Who asked. */
  time_t hr_born;                   /**< When they connected. */
  char hr_target[CHANNELLEN + 1];   /**< What they named. */
  char hr_msgid[MSGIDLEN + 1];      /**< The message. */
  char hr_reason[TOPICLEN + 1];     /**< Why, or "". */
};

/** The client that asked, or NULL if it is not there any more. */
static struct Client* hist_redactor(const struct HistRedact* rd)
{
  struct Client* cptr = findNUser(rd->hr_numnick);

  if (!cptr || !MyUser(cptr) || cli_firsttime(cptr) != rd->hr_born)
    return 0;

  return cptr;
}

/** Refuse, in the specification's terms. */
static void hist_redact_fail(struct Client* cptr, const char* code,
                             const char* context, const char* text)
{
  send_fail(cptr, MSG_REDACT, code, context, _(cptr, text));
}

/** Relay a redaction to everybody who can make sense of it.
 *
 * Only to clients that asked for the capability: to anybody else REDACT
 * is a command they have never heard of, and a line they cannot parse is
 * not an improvement on a message they still have.  What they keep is
 * what they were shown, which is the honest outcome for a client that
 * does not implement this.
 */
static void hist_redact_relay(struct Client* sptr, const char* target,
                              const char* msgid, const char* reason)
{
  struct Channel* chptr = IsChannelName(target) ? FindChannel(target) : 0;
  const char* text = (reason && *reason) ? reason : 0;

  if (chptr) {
    if (text)
      sendcmdto_capflag_channel_butserv_butone(sptr, MSG_REDACT, TOK_REDACT,
                                               chptr, 0, 0, hist_redact_cap,
                                               CAP_NONE, "%H %s :%s", chptr,
                                               msgid, text);
    else
      sendcmdto_capflag_channel_butserv_butone(sptr, MSG_REDACT, TOK_REDACT,
                                               chptr, 0, 0, hist_redact_cap,
                                               CAP_NONE, "%H %s", chptr,
                                               msgid);

    sendcmdto_channel_servers_butone(sptr, MSG_REDACT, TOK_REDACT, chptr,
                                     cli_from(sptr), 0,
                                     text ? "%H %s :%s" : "%H %s", chptr,
                                     msgid, text);
    return;
  }

  /* A direct message: the other end, and the server it is on. */
  {
    struct Client* acptr = FindUser(target);

    if (!acptr)
      return;

    if (MyUser(acptr)) {
      if (!CapActive(acptr, hist_redact_cap))
        return;
      sendcmdto_one(sptr, MSG_REDACT, TOK_REDACT, acptr,
                    text ? "%C %s :%s" : "%C %s", acptr, msgid, text);
    } else {
      sendcmdto_one(sptr, MSG_REDACT, TOK_REDACT, acptr,
                    text ? "%C %s :%s" : "%C %s", acptr, msgid, text);
    }
  }
}

/** The deletion came back. */
static void hist_redact_deleted(long long rows, void* user)
{
  struct HistRedact* rd = (struct HistRedact*) user;
  struct Client* cptr = hist_redactor(rd);

  if (!cptr) {
    MyFree(rd);
    return;
  }

  if (rows < 0) {
    hist_redact_fail(cptr, "REDACT_FAILED", rd->hr_msgid,
                     N_("The message could not be deleted"));
    MyFree(rd);
    return;
  }

  /* Only now.  A client that removed the message from its view while the
   * store kept it would be the one outcome worse than not supporting
   * this at all.
   *
   * The fan-out includes the person who asked, which is deliberate: they
   * are being told the redaction happened, not echoed their own command,
   * and a client that has to guess whether its request went through is
   * one that will ask again. */
  hist_redact_relay(cptr, rd->hr_target, rd->hr_msgid, rd->hr_reason);

  MyFree(rd);
}

/** Non-zero if \a cptr may take back a message \a found describes. */
static int hist_may_redact(struct Client* cptr, const struct HistFound* found)
{
  struct Channel* chptr;
  char mine[NICKLEN + 1];

  /* An operator who can delete an account's whole record can certainly
   * delete one message of it. */
  if (HasPriv(cptr, PRIV_HISTORY))
    return 1;

  if (found->hf_channel) {
    chptr = FindChannel(found->hf_target);

    /* Ops, with no window: moderating is not undoing, and a moderator
     * who had to be quick about it would not be one. */
    if (chptr && is_chan_op(cptr, chptr))
      return 1;
  }

  if (!IsAccount(cptr) || !cli_user(cptr)->account[0])
    return 0;

  hist_canon(mine, sizeof(mine), cli_user(cptr)->account);

  if (ircd_strcmp(mine, found->hf_account))
    return 0;

  /* Their own, so the only question left is how long ago.  The window is
   * measured against the message's own timestamp, which is the network's
   * and not this server's clock. */
  {
    int window = feature_int(FEAT_HISTORY_REDACT_WINDOW);
    char now[40];

    if (window <= 0)
      return 1;

    hist_time_ago(now, sizeof(now), window);

    /* Both are ISO 8601 with the same shape, so they order as text. */
    return strcmp(found->hf_time, now) >= 0;
  }
}

/** The lookup came back. */
static void hist_redact_found(const struct HistFound* found, void* user)
{
  struct HistRedact* rd = (struct HistRedact*) user;
  struct Client* cptr = hist_redactor(rd);
  char canon[CHANNELLEN + 1];

  if (!cptr) {
    MyFree(rd);
    return;
  }

  if (!found) {
    hist_redact_fail(cptr, "UNKNOWN_MSGID", rd->hr_msgid,
                     N_("No such message, or it is no longer stored"));
    MyFree(rd);
    return;
  }

  /* The target has to be the one the message was actually in.  Otherwise
   * naming any channel you are an operator of would let you delete a
   * message from any other. */
  hist_canon(canon, sizeof(canon), rd->hr_target);

  if (ircd_strcmp(canon, found->hf_canon)) {
    hist_redact_fail(cptr, "UNKNOWN_MSGID", rd->hr_msgid,
                     N_("That message was not sent to that target"));
    MyFree(rd);
    return;
  }

  if (!hist_may_redact(cptr, found)) {
    hist_redact_fail(cptr, "REDACT_FORBIDDEN", rd->hr_msgid,
                     N_("You may not take that message back"));
    MyFree(rd);
    return;
  }

  if (!hist_store_redact(rd->hr_msgid, hist_redact_deleted, rd)) {
    hist_redact_fail(cptr, "REDACT_FAILED", rd->hr_msgid,
                     N_("The store is not available right now"));
    MyFree(rd);
  }
}

/** REDACT from a client.
 * @param[in] cptr Connection it arrived on.
 * @param[in] sptr Who sent it.
 * @param[in] parc Parameter count.
 * @param[in] parv Parameters.
 * @return Zero.
 */
int hist_m_redact(struct Client* cptr, struct Client* sptr, int parc,
                  char* parv[])
{
  struct HistRedact* rd;

  if (parc < 3 || EmptyString(parv[1]) || EmptyString(parv[2]))
    return need_more_params(sptr, MSG_REDACT);

  /* Being able to see the target is the cheap half of the question, and
   * the only half this server can answer without asking the store. */
  if (IsChannelName(parv[1])) {
    struct Channel* chptr = FindChannel(parv[1]);

    if (!chptr || !find_channel_member(sptr, chptr)) {
      hist_redact_fail(sptr, "UNKNOWN_TARGET", parv[1],
                       N_("You are not on that channel"));
      return 0;
    }
  } else if (!FindUser(parv[1])) {
    hist_redact_fail(sptr, "UNKNOWN_TARGET", parv[1],
                     N_("No such nickname"));
    return 0;
  }

  rd = (struct HistRedact*) MyCalloc(1, sizeof(*rd));
  ircd_snprintf(0, rd->hr_numnick, sizeof(rd->hr_numnick), "%s%s",
                NumNick(sptr));
  rd->hr_born = cli_firsttime(sptr);
  ircd_strncpy(rd->hr_target, parv[1], sizeof(rd->hr_target) - 1);
  ircd_strncpy(rd->hr_msgid, parv[2], sizeof(rd->hr_msgid) - 1);
  if (parc > 3 && !EmptyString(parv[3]))
    ircd_strncpy(rd->hr_reason, parv[3], sizeof(rd->hr_reason) - 1);

  if (!hist_store_find(rd->hr_msgid, hist_redact_found, rd)) {
    hist_redact_fail(sptr, "REDACT_FAILED", rd->hr_msgid,
                     N_("The store is not available right now"));
    MyFree(rd);
  }

  return 0;
}

/** REDACT from another server.
 *
 * Every server runs this module against the same store, so the deletion
 * has happened already and there is nothing to check: what arrives is an
 * announcement, and this server passes it on to its own clients and its
 * own peers.
 */
int hist_ms_redact(struct Client* cptr, struct Client* sptr, int parc,
                   char* parv[])
{
  if (parc < 3 || EmptyString(parv[1]) || EmptyString(parv[2]))
    return 0;

  hist_redact_relay(sptr, parv[1], parv[2],
                    (parc > 3) ? parv[3] : 0);

  return 0;
}
