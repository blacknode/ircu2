/*
 * IRC - Internet Relay Chat, modules/services/history/hist_capture.c
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
 * @brief What is worth storing, decided once.
 *
 * The hook fires for everything this server relays.  This file is the one
 * place that says which of it is a conversation, and hist_store.c is the
 * one place that says how a conversation is written down.
 */
#include "config.h"

#include "channel.h"
#include "client.h"
#include "hooks.h"
#include "history.h"
#include "ircd_chattr.h"
#include "ircd_features.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "msg_tag.h"
#include "parse.h"
#include "ircd.h"
#include "struct.h"

#include <time.h>

#include <string.h>

void hist_canon(char* buf, size_t buflen, const char* name)
{
  size_t i;

  if (!buf || buflen == 0)
    return;

  if (!name) {
    buf[0] = '\0';
    return;
  }

  for (i = 0; i + 1 < buflen && name[i]; i++)
    buf[i] = ToLower(name[i]);

  buf[i] = '\0';
}

/** The account a client has proved, or NULL.
 *
 * This is the account the network's services vouched for, which is the
 * only name a message can safely be filed under.  A nickname nobody
 * proved is one somebody else may be wearing tomorrow.
 */
static const char* hist_account_of(struct Client* cptr)
{
  if (!cptr || !IsAccount(cptr) || !cli_user(cptr))
    return 0;

  if (!cli_user(cptr)->account[0])
    return 0;

  return cli_user(cptr)->account;
}

void hist_time_ago(char* buf, size_t buflen, int seconds)
{
  time_t when = CurrentTime - (time_t) seconds;
  struct tm tm;

  gmtime_r(&when, &tm);
  ircd_snprintf(0, buf, buflen, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec);
}

/** One client-only tag of the line being handled, or NULL.
 *
 * The line is still the one the hook fired for -- the hook runs inside
 * the handler -- so the tags it arrived with are still the parser's.
 * CLIENTTAGDENY decides whether it may be relayed at all; a tag the
 * network refuses to carry is not one this module should write down
 * either, so the same test gates both.
 */
static const char* hist_client_tag(const char* key)
{
  struct MsgTag* tag = msg_tag_find(parse_tags(), key);

  if (!tag || !msg_tag_client_allowed(key))
    return 0;

  return tag->value;
}

/** The prefix a client is wearing, nick!user@host.
 *
 * The visible host, which for every user is the cipher of their address
 * (doc/readme.accounting): a history that recorded the real one would be
 * a permanent record of what +x exists to hide.
 */
static void hist_prefix_of(char* buf, size_t buflen, struct Client* cptr)
{
  buf[0] = '\0';

  if (!cptr || !cli_user(cptr))
    return;

  ircd_snprintf(0, buf, buflen, "%s!%s@%s", cli_name(cptr),
                cli_user(cptr)->username, cli_user(cptr)->host);
}

/** Non-zero if \a text is a CTCP that is not an ACTION.
 *
 * An ACTION is somebody speaking and belongs in the history.  A VERSION,
 * a PING or a DCC handshake is two clients talking to each other about
 * themselves, and storing it would fill the record with things nobody
 * said.
 */
static int hist_is_ctcp_noise(const char* text)
{
  if (!text || text[0] != '\001')
    return 0;

  return 0 != ircd_strncmp(text + 1, "ACTION", 6);
}

enum HookResult hist_capture(struct HookContext* ctx, void* user)
{
  const struct HookMessage* hm;
  struct HistMessage msg;
  const char* reply;
  const char* react = 0;
  char prefix[NICKLEN + USERLEN + HOSTLEN + 3];
  char canon[CHANNELLEN + 1];
  char from_canon[ACCOUNTLEN + 1];
  char to_canon[ACCOUNTLEN + 1];
  const char* sender_account;
  const char* recipient_account;

  (void) user;

  hm = ctx->hc_message;

  if (!hm)
    return HOOK_CONTINUE;

  reply = hist_client_tag(HIST_TAG_REPLY);

  /* A TAGMSG carries tags and no text of its own, so most of them are
   * nothing to keep: a typing indicator is true for four seconds and then
   * is not, and a row saying somebody was typing in March is not history.
   *
   * A reaction is the exception, and it is not really an exception: it has
   * its own identifier, its own time, and it says something about a
   * message that is still there.  It is a message, so it is stored like
   * one -- the reaction in the body and what it is about in reply_to --
   * which is what makes removing one and reading them back in order the
   * same operations as for anything else.
   */
  if (hm->hmm_kind == HOOK_MSG_TAGMSG) {
    react = hist_client_tag(HIST_TAG_REACT);

    if (!react || !*react || !reply || !*reply)
      return HOOK_CONTINUE;

    if (strlen(react) > HIST_REACT_MAX)
      return HOOK_CONTINUE;
  }

  if (hist_is_ctcp_noise(ctx->hc_arg))
    return HOOK_CONTINUE;

  memset(&msg, 0, sizeof(msg));

  /* Stored with the message, because a transcript has to be
   * self-contained: it is shown back weeks later, when the sender may
   * have changed nickname, changed host or never come back, and building
   * a prefix then from whoever holds that nickname would put the words in
   * the wrong mouth.
   */
  hist_prefix_of(prefix, sizeof(prefix), ctx->hc_source);
  msg.hm_prefix = prefix[0] ? prefix : 0;

  msg.hm_msgid = hm->hmm_msgid;
  msg.hm_time = hm->hmm_time;

  switch (hm->hmm_kind) {
  case HOOK_MSG_NOTICE: msg.hm_kind = HIST_NOTICE; break;
  case HOOK_MSG_TAGMSG: msg.hm_kind = HIST_TAGMSG; break;
  default:              msg.hm_kind = HIST_PRIVMSG; break;
  }

  /* A reaction's body is the reaction: a TAGMSG has no text of its own,
   * and what it came to say is in the tag. */
  msg.hm_body = react ? react : ctx->hc_arg;
  msg.hm_reply = (reply && *reply) ? reply : 0;

  sender_account = hist_account_of(ctx->hc_source);

  if (ctx->hc_channel) {
    msg.hm_channel = 1;
    msg.hm_target = ctx->hc_channel->chname;
    hist_canon(canon, sizeof(canon), ctx->hc_channel->chname);
    msg.hm_canon = canon;
    msg.hm_sender = ctx->hc_source ? cli_name(ctx->hc_source) : "*";
    msg.hm_account = 0;
    if (sender_account) {
      hist_canon(from_canon, sizeof(from_canon), sender_account);
      msg.hm_account = from_canon;
    }
    msg.hm_recipient = 0;
  } else {
    if (!feature_bool(FEAT_HISTORY_PRIVATE))
      return HOOK_CONTINUE;

    recipient_account = hist_account_of(ctx->hc_client);

    /* Both ends have to have proved their nicknames, and not because a
     * guest deserves less.  A direct message is shown back to the two
     * people in it and to nobody else, and the only handle on a person
     * this server has is the account the services vouched for: filing a message
     * under a bare nickname would show it to whoever is wearing that
     * nickname next week.  With nobody it can safely be shown to, there
     * is no reason to keep it.
     */
    if (!sender_account || !recipient_account)
      return HOOK_CONTINUE;

    msg.hm_channel = 0;
    msg.hm_target = ctx->hc_client ? cli_name(ctx->hc_client) : "*";
    hist_canon(canon, sizeof(canon), msg.hm_target);
    msg.hm_canon = canon;
    msg.hm_sender = ctx->hc_source ? cli_name(ctx->hc_source) : "*";

    /* Canonical, because this is what a later query matches on.  A user
     * wears the nickname they typed -- "Alice" one day, "alice" the next
     * -- and the account is that nickname, so two spellings of the same
     * person would file their conversation in two places.
     */
    hist_canon(from_canon, sizeof(from_canon), sender_account);
    hist_canon(to_canon, sizeof(to_canon), recipient_account);
    msg.hm_account = from_canon;
    msg.hm_recipient = to_canon;
  }

  hist_store_write(&msg);

  return HOOK_CONTINUE;
}
