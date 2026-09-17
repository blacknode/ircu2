/*
 * IRC - Internet Relay Chat, modules/services/history/hist_edit.c
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
 * @brief EDIT: changing what a message says.
 *
 * The other half of REDACT, and it lives here for the same reason: **you
 * cannot edit what nobody stored**.  A server with no history has no copy
 * to change and no way to know who wrote the message being pointed at, so
 * the command would be an announcement that something now reads
 * differently with nothing having been changed.  Nothing is relayed until
 * the row has been updated -- a client showing the new text while the
 * store still holds the old one is the one outcome worse than not having
 * this at all.
 *
 * **The identifier does not change**, which is the whole reason an edit is
 * not simply another message.  The replies and the reactions point at it,
 * the read markers are ordered against it, and a message that was edited
 * is still the message they are about.
 *
 * **Only the author, ever.**  A channel operator may REDACT, because
 * moderating is taking something out of the room; nobody may EDIT
 * somebody else's message, because that is putting words in their mouth
 * under their own prefix, and there is no version of that which is
 * moderation.  The window (@c HISTORY_EDIT_WINDOW) is shorter than the
 * redaction one on purpose: a redaction leaves a hole everybody can see,
 * while a change leaves a sentence nobody can tell was ever different.
 *
 * An empty new text is refused rather than treated as a deletion.  There
 * is a command for deleting a message and this is not it -- and an edit
 * that emptied a message would be a redaction that left the row, the
 * reactions and the replies pointing at nothing.
 */
#include "config.h"

#include "capab.h"
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "history.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_i18n.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "msg.h"
#include "msgid.h"
#include "numnicks.h"
#include "s_user.h"
#include "send.h"
#include "struct.h"

#include <string.h>

/** One edit waiting on the store. */
struct HistEdit {
  char he_numnick[10];             /**< Who asked. */
  time_t he_born;                  /**< When they connected. */
  char he_target[CHANNELLEN + 1];  /**< What they named. */
  char he_msgid[MSGIDLEN + 1];     /**< The message. */
  char he_time[40];                /**< When it was sent, from the row. */
  char he_body[BUFSIZE];           /**< What it says now. */
};

/** The client that asked, or NULL if it is not there any more. */
static struct Client* hist_editor(const struct HistEdit* ed)
{
  struct Client* cptr = findNUser(ed->he_numnick);

  if (!cptr || !MyUser(cptr) || cli_firsttime(cptr) != ed->he_born)
    return 0;

  return cptr;
}

/** Refuse, in the terms the standard replies use. */
static void hist_edit_fail(struct Client* cptr, const char* code,
                           const char* context, const char* text)
{
  send_fail(cptr, MSG_EDIT, code, context, _(cptr, text));
}

/** Tell everybody who can make sense of it that the message changed.
 *
 * Only clients that asked for the capability, for REDACT's reason: to
 * anybody else EDIT is a command they have never heard of, and a line
 * they cannot parse is not an improvement on the text they already have.
 */
static void hist_edit_relay(struct Client* sptr, const char* target,
                            const char* msgid, const char* body)
{
  struct Channel* chptr = IsChannelName(target) ? FindChannel(target) : 0;

  if (chptr) {
    sendcmdto_capflag_channel_butserv_butone(sptr, MSG_EDIT, TOK_EDIT, chptr,
                                             0, 0, hist_edit_cap, CAP_NONE,
                                             "%H %s :%s", chptr, msgid, body);

    sendcmdto_channel_servers_butone(sptr, MSG_EDIT, TOK_EDIT, chptr,
                                     cli_from(sptr), 0, "%H %s :%s", chptr,
                                     msgid, body);
    return;
  }

  {
    struct Client* acptr = FindUser(target);

    if (!acptr)
      return;

    if (MyUser(acptr) && !CapActive(acptr, hist_edit_cap))
      return;

    sendcmdto_one(sptr, MSG_EDIT, TOK_EDIT, acptr, "%C %s :%s", acptr, msgid,
                  body);
  }
}

/** The update came back. */
static void hist_edit_written(long long rows, void* user)
{
  struct HistEdit* ed = (struct HistEdit*) user;
  struct Client* cptr = hist_editor(ed);

  if (!cptr) {
    MyFree(ed);
    return;
  }

  if (rows < 0) {
    hist_edit_fail(cptr, "EDIT_FAILED", ed->he_msgid,
                   N_("The message could not be changed"));
    MyFree(ed);
    return;
  }

  if (rows == 0) {
    /* It was there a round trip ago and is not now: a purge, a
     * redaction, or the other end of a race with one. */
    hist_edit_fail(cptr, "UNKNOWN_MSGID", ed->he_msgid,
                   N_("No such message, or it is no longer stored"));
    MyFree(ed);
    return;
  }

  /* Only now, and including the person who asked: they are being told the
   * edit happened rather than echoed their own command, and a client that
   * has to guess whether its request went through is one that will send
   * it again. */
  hist_edit_relay(cptr, ed->he_target, ed->he_msgid, ed->he_body);

  MyFree(ed);
}

/** Non-zero if \a cptr may rewrite the message \a found describes. */
static int hist_may_edit(struct Client* cptr, const struct HistFound* found)
{
  char mine[NICKLEN + 1];
  int window;
  char now[40];

  /* Nobody else, whatever they are: an operator with history_admin can
   * delete this message and an operator of the channel can too, but
   * neither of them may make it say something its author did not. */
  if (!IsAccount(cptr) || !cli_user(cptr)->account[0])
    return 0;

  hist_canon(mine, sizeof(mine), cli_user(cptr)->account);

  if (ircd_strcmp(mine, found->hf_account))
    return 0;

  window = feature_int(FEAT_HISTORY_EDIT_WINDOW);

  if (window <= 0)
    return 1;

  /* Measured against the message's own timestamp, which is the network's
   * clock reading and not this server's. */
  hist_time_ago(now, sizeof(now), window);

  return strcmp(found->hf_time, now) >= 0;
}

/** The lookup came back. */
static void hist_edit_found(const struct HistFound* found, void* user)
{
  struct HistEdit* ed = (struct HistEdit*) user;
  struct Client* cptr = hist_editor(ed);
  char canon[CHANNELLEN + 1];

  if (!cptr) {
    MyFree(ed);
    return;
  }

  if (!found) {
    hist_edit_fail(cptr, "UNKNOWN_MSGID", ed->he_msgid,
                   N_("No such message, or it is no longer stored"));
    MyFree(ed);
    return;
  }

  /* The target has to be the one the message was really sent to, for the
   * reason REDACT checks the same thing: otherwise naming a channel you
   * are in would reach a message that was said in another. */
  hist_canon(canon, sizeof(canon), ed->he_target);

  if (ircd_strcmp(canon, found->hf_canon)) {
    hist_edit_fail(cptr, "UNKNOWN_MSGID", ed->he_msgid,
                   N_("That message was not sent to that target"));
    MyFree(ed);
    return;
  }

  /* A reaction has no text of its own -- what a TAGMSG carries is the
   * reaction itself -- so there is nothing here to rewrite.  Taking one
   * back and adding another is what changing a reaction is. */
  if (found->hf_kind == HIST_TAGMSG) {
    hist_edit_fail(cptr, "INVALID_TARGET", ed->he_msgid,
                   N_("That message has no text to change"));
    MyFree(ed);
    return;
  }

  if (!hist_may_edit(cptr, found)) {
    hist_edit_fail(cptr, "EDIT_FORBIDDEN", ed->he_msgid,
                   N_("You may not change that message"));
    MyFree(ed);
    return;
  }

  ircd_strncpy(ed->he_time, found->hf_time, sizeof(ed->he_time) - 1);

  if (!hist_store_edit(ed->he_msgid, ed->he_time, ed->he_body,
                       hist_edit_written, ed)) {
    hist_edit_fail(cptr, "EDIT_FAILED", ed->he_msgid,
                   N_("The store is not available right now"));
    MyFree(ed);
  }
}

/** EDIT from a client.
 *
 * @verbatim
 *   EDIT <target> <msgid> :<new text>
 * @endverbatim
 *
 * @param[in] cptr Connection it arrived on.
 * @param[in] sptr Who sent it.
 * @param[in] parc Parameter count.
 * @param[in] parv Parameters.
 * @return Zero.
 */
int hist_m_edit(struct Client* cptr, struct Client* sptr, int parc,
                char* parv[])
{
  struct HistEdit* ed;

  if (parc < 4 || EmptyString(parv[1]) || EmptyString(parv[2]))
    return need_more_params(sptr, MSG_EDIT);

  /* Not a deletion.  There is a command for that and this is not it. */
  if (EmptyString(parv[3])) {
    hist_edit_fail(sptr, "INVALID_PARAMS", parv[2],
                   N_("An edit needs text; use REDACT to take a message "
                      "back"));
    return 0;
  }

  /* Being able to see the target is the cheap half of the question and
   * the only half this server can answer without asking the store. */
  if (IsChannelName(parv[1])) {
    struct Channel* chptr = FindChannel(parv[1]);

    if (!chptr || !find_channel_member(sptr, chptr)) {
      hist_edit_fail(sptr, "UNKNOWN_TARGET", parv[1],
                     N_("You are not on that channel"));
      return 0;
    }
  } else if (!FindUser(parv[1])) {
    hist_edit_fail(sptr, "UNKNOWN_TARGET", parv[1], N_("No such nickname"));
    return 0;
  }

  ed = (struct HistEdit*) MyCalloc(1, sizeof(*ed));
  ircd_snprintf(0, ed->he_numnick, sizeof(ed->he_numnick), "%s%s",
                NumNick(sptr));
  ed->he_born = cli_firsttime(sptr);
  ircd_strncpy(ed->he_target, parv[1], sizeof(ed->he_target) - 1);
  ircd_strncpy(ed->he_msgid, parv[2], sizeof(ed->he_msgid) - 1);
  ircd_strncpy(ed->he_body, parv[3], sizeof(ed->he_body) - 1);

  if (!hist_store_find(ed->he_msgid, hist_edit_found, ed)) {
    hist_edit_fail(sptr, "EDIT_FAILED", ed->he_msgid,
                   N_("The store is not available right now"));
    MyFree(ed);
  }

  return 0;
}

/** EDIT from another server.
 *
 * Every server runs this module against the same store, so the change has
 * happened already and there is nothing to check: what arrives is an
 * announcement, which this server passes on to its own clients and its
 * own peers.
 */
int hist_ms_edit(struct Client* cptr, struct Client* sptr, int parc,
                 char* parv[])
{
  if (parc < 4 || EmptyString(parv[1]) || EmptyString(parv[2])
      || EmptyString(parv[3]))
    return 0;

  hist_edit_relay(sptr, parv[1], parv[2], parv[3]);

  return 0;
}
