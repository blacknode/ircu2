/*
 * IRC - Internet Relay Chat, ircd/ircd_relay.c
 * Copyright (C) 1990 Jarkko Oikarinen and
 *                    University of Oulu, Computing Center
 *
 * See file AUTHORS in IRC package for additional names of
 * the programmers.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
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
 * @brief Helper functions to relay various types of messages.
 * @version $Id$
 *
 * There are four basic types of messages, each with four subtypes.
 *
 * The basic types are: channel, directed, masked, and private.
 * Channel messages are (perhaps obviously) sent directly to a
 * channel.  Directed messages are sent to "NICK[%host]@server", but
 * only allowed if the server is a services server (to avoid
 * information leaks for normal clients).  Masked messages are sent to
 * either *@*host.mask or *.server.mask.  Private messages are sent to
 * NICK.
 *
 * The subtypes for each type are: client message, client notice,
 * server message, and server notice.  Client subtypes are sent by a
 * local user, and server subtypes are given to us by a server.
 * Notice subtypes correspond to the NOTICE command, and message
 * subtypes correspond to the PRIVMSG command.
 *
 * As a special note, directed messages do not have server subtypes,
 * since there is no difference in handling them based on origin.
 */
#include "config.h"

#include "ircd_relay.h"
#include "bot.h"
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "hooks.h"
#include "ircd.h"
#include "ircd_chattr.h"
#include "ircd_features.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "match.h"
#include "msg.h"
#include "msg_tag.h"
#include "numeric.h"
#include "numnicks.h"
#include "s_debug.h"
#include "s_misc.h"
#include "s_user.h"
#include "send.h"
#include "sline.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * This file contains message relaying functions for client and server
 * private messages and notices
 * TODO: This file contains a lot of cut and paste code, and needs
 * to be cleaned up a bit. The idea is to factor out the common checks
 * but not introduce any IsOper/IsUser/MyUser/IsServer etc. stuff.
 */

/** Tell the modules a message went out on this server.
 *
 * #HOOK_MESSAGE_DELIVERED fires from every relay path, the ones that take
 * a local client's message and the ones that take a link's, which is what
 * makes it usable by a module that stores what was said: the message hooks
 * that were here before only ever saw what started on this server.
 *
 * It runs after the send, so nothing it does can change the message; and
 * it is guarded on a hook being registered, because asking for the line's
 * identifier is what creates one, and a server with nothing listening has
 * no use for a name nobody will ever say.
 *
 * Two things are deliberately never reported.  A message **to a service**
 * (+S or +k) is not: that is where a password goes -- IDENTIFY, REGISTER,
 * DROP -- and handing it to whatever is listening would put credentials in
 * a log or a database with nothing to stop it.  The module the service
 * belongs to already gets the message through #HOOK_MESSAGE_RECEIVED, and
 * decides for itself.  A **masked** message ($#mask, $host) is not either:
 * an operator addressing everyone on a server is an announcement, not a
 * conversation, and it has no target anything could file it under.
 *
 * @param[in] sptr Who sent the message.
 * @param[in] acptr User it was addressed to, or NULL for a channel.
 * @param[in] chptr Channel it was addressed to, or NULL for a user.
 * @param[in] text The text, or "" for a TAGMSG.
 * @param[in] kind PRIVMSG, NOTICE or TAGMSG.
 * @param[in] target The target as the sender wrote it.
 */
static void relay_delivered(struct Client* sptr, struct Client* acptr,
                            struct Channel* chptr, const char* text,
                            enum HookMsgKind kind, const char* target)
{
  struct HookContext hc;
  struct HookMessage hm;
  char stamp[32];
  const char* tok;

  if (!hook_is_active(HOOK_MESSAGE_DELIVERED))
    return;

  if (acptr && (IsServiceBot(acptr) || IsChannelService(acptr)))
    return;

  switch (kind) {
  case HOOK_MSG_NOTICE:  tok = TOK_NOTICE; break;
  case HOOK_MSG_TAGMSG:  tok = TOK_TAGMSG; break;
  default:               tok = TOK_PRIVATE; break;
  }

  msg_tag_line_time(stamp, sizeof(stamp));

  hm.hmm_kind = kind;
  hm.hmm_msgid = msg_tag_line_msgid(tok);
  hm.hmm_time = stamp;
  hm.hmm_target = target;
  hm.hmm_remote = !MyUser(sptr);

  hook_context_init(&hc);
  hc.hc_client = acptr;
  hc.hc_source = sptr;
  hc.hc_channel = chptr;
  hc.hc_arg = text ? text : "";
  hc.hc_notice = (kind == HOOK_MSG_NOTICE);
  hc.hc_message = &hm;

  hook_run(HOOK_MESSAGE_DELIVERED, &hc);
}

/** Deliver one message to a channel as two bodies.
 *
 * What #HookContext::hc_alt is for: a module has given the message a
 * content type, and the clients that never agreed to that type have to be
 * sent something they can read.  So the channel hears it twice, with the
 * capability deciding which half hears which -- and the links hear the
 * rich body, because the next server has the same choice to make and
 * cannot make it from the plain one.
 *
 * What is stored, and what a service bot is handed, is the **alternative**.
 * A transcript is read back by whoever reads it, and the one body it can
 * keep is the one everybody can read.
 *
 * @param[in] sptr Who sent it.
 * @param[in] chptr The channel.
 * @param[in] cmd Command name (PRIVMSG or NOTICE).
 * @param[in] tok Its P10 token.
 * @param[in] cap Capability that decides which body a client gets.
 * @param[in] rich The body for clients that have it.
 * @param[in] alt The body for everybody else.
 * @param[in] alt_tag A client tag not to relay with \a alt, or NULL.
 */
static void relay_channel_two_ways(struct Client* sptr, struct Channel* chptr,
                                   const char* cmd, const char* tok, int cap,
                                   const char* rich, const char* alt,
                                   const char* alt_tag)
{
  int notice = !strcmp(cmd, MSG_NOTICE);

  sendcmdto_capflag_channel_butserv_butone(sptr, cmd, tok, chptr,
                                           cli_from(sptr),
                                           SKIP_DEAF | SKIP_BURST,
                                           cap, CAP_NONE, "%H :%s", chptr,
                                           rich);

  /* The tag that says what the rich body is would be a lie on the other
   * one, and a tag that is a lie is worse than no tag. */
  msg_tag_suppress(alt_tag);
  sendcmdto_capflag_channel_butserv_butone(sptr, cmd, tok, chptr,
                                           cli_from(sptr),
                                           SKIP_DEAF | SKIP_BURST,
                                           CAP_NONE, cap, "%H :%s", chptr,
                                           alt);
  msg_tag_suppress(0);

  sendcmdto_channel_servers_butone(sptr, cmd, tok, chptr, cli_from(sptr),
                                   SKIP_BURST, "%H :%s", chptr, rich);

  bot_deliver_channel(sptr, chptr, notice, alt);

  if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE)) {
    int mine = CapHas(cli_active(sptr), cap);

    if (!mine)
      msg_tag_suppress(alt_tag);
    sendcmdto_one(sptr, cmd, tok, cli_from(sptr), "%H :%s", chptr,
                  mine ? rich : alt);
    msg_tag_suppress(0);
  }

  relay_delivered(sptr, 0, chptr, alt,
                  notice ? HOOK_MSG_NOTICE : HOOK_MSG_PRIVMSG,
                  chptr->chname);
}

/** Relay a local user's message to a channel.
 * Generates an error if the client cannot send to the channel.
 * @param[in] sptr Client that originated the message.
 * @param[in] name Name of target channel.
 * @param[in] text %Message to relay.
 */
void relay_channel_message(struct Client* sptr, const char* name, const char* text)
{
  struct Channel* chptr;
  struct Membership* memb;
  const char *ch;

  assert(0 != sptr);
  assert(0 != name);
  assert(0 != text);

  if (0 == (chptr = FindChannel(name))) {
    send_reply(sptr, ERR_NOSUCHCHANNEL, name);
    return;
  }
  /*
   * This first: Almost never a server/service
   */
  if (!client_can_send_to_channel(sptr, chptr, 0)) {
    send_reply(sptr, ERR_CANNOTSENDTOCHAN, chptr->chname);
    return;
  }
  if ((chptr->mode.mode & MODE_NOPRIVMSGS) &&
      check_target_limit(sptr, NULL, chptr))
    return;
  memb = find_member_link(chptr, sptr);
  if (memb && IsDelayedTarget(memb))
    ClearDelayedTarget(memb);

  if (chptr->mode.mode & MODE_NOCOLOR) {
    for (ch = text; *ch != '\0'; ++ch) {
      if (*ch == 3 || *ch == 27) {
        send_reply(sptr, ERR_CANNOTSENDTOCHAN, chptr->chname);
        return;
      }
    }
  }

  if ((chptr->mode.mode & MODE_NOCTCP) && ircd_strncmp(text, "\001ACTION ", 8)) {
    for (ch = text; *ch != '\0'; ++ch) {
      if (*ch == 1) {
        send_reply(sptr, ERR_CANNOTSENDTOCHAN, chptr->chname);
        return;
      }
    }
  }

  if (sline_check_chanmsg(sptr, chptr, text, MSG_PRIVATE)) {
    return;
  }

  /* Last stop before delivery, so every other check has already had its
   * say and a module sees the text as it would actually be sent.  A hook
   * may rewrite it into the buffer the server provides; it must not hand
   * back a pointer of its own.
   */
  if (hook_is_active(HOOK_MESSAGE_PRE_CHANNEL)) {
    struct HookContext hc;
    char rewrite[BUFSIZE];
    char alt[BUFSIZE];

    hook_context_init(&hc);
    hc.hc_client = sptr;
    hc.hc_source = sptr;
    hc.hc_channel = chptr;
    hc.hc_arg = text;
    hc.hc_rewrite = rewrite;
    hc.hc_rewrite_len = sizeof(rewrite);
    hc.hc_alt = alt;
    hc.hc_alt_len = sizeof(alt);
    hc.hc_alt_cap = CAP_NONE;
    rewrite[0] = '\0';
    alt[0] = '\0';

    if (hook_run(HOOK_MESSAGE_PRE_CHANNEL, &hc) == HOOK_DENY) {
      hook_deny_reply(sptr, &hc, ERR_CANNOTSENDTOCHAN, chptr->chname);
      return;
    }

    if (hc.hc_rewritten) {
      rewrite[sizeof(rewrite) - 1] = '\0';
      text = rewrite;
    }

    if (hc.hc_alt_set && hc.hc_alt_cap != CAP_NONE) {
      alt[sizeof(alt) - 1] = '\0';
      RevealDelayedJoinIfNeeded(sptr, chptr);
      relay_channel_two_ways(sptr, chptr, CMD_PRIVATE, hc.hc_alt_cap,
                             text, alt, hc.hc_alt_tag);
      return;
    }
  }

  RevealDelayedJoinIfNeeded(sptr, chptr);
  sendcmdto_channel_butone(sptr, CMD_PRIVATE, chptr, cli_from(sptr),
			   SKIP_DEAF | SKIP_BURST, "%H :%s", chptr, text);
  bot_deliver_channel(sptr, chptr, 0, text);

  if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
    sendcmdto_one(sptr, CMD_PRIVATE, cli_from(sptr), "%H :%s", chptr, text);

  relay_delivered(sptr, 0, chptr, text, HOOK_MSG_PRIVMSG, chptr->chname);
}

/** Relay a local user's notice to a channel.
 * Silently exits if the client cannot send to the channel.
 * @param[in] sptr Client that originated the message.
 * @param[in] name Name of target channel.
 * @param[in] text %Message to relay.
 */
void relay_channel_notice(struct Client* sptr, const char* name, const char* text)
{
  struct Channel* chptr;
  struct Membership* memb;
  const char *ch;

  assert(0 != sptr);
  assert(0 != name);
  assert(0 != text);

  if (0 == (chptr = FindChannel(name)))
    return;
  /*
   * This first: Almost never a server/service
   */
  if (!client_can_send_to_channel(sptr, chptr, 0))
    return;

  if ((chptr->mode.mode & MODE_NOPRIVMSGS) &&
      check_target_limit(sptr, NULL, chptr))
    return;
  memb = find_member_link(chptr, sptr);
  if (memb && IsDelayedTarget(memb))
    ClearDelayedTarget(memb);

  if (chptr->mode.mode & MODE_NOCOLOR) {
    for (ch = text; *ch != '\0'; ++ch) {
      if (*ch == 3 || *ch == 27) {
        send_reply(sptr, ERR_CANNOTSENDTOCHAN, chptr->chname);
        return;
      }
    }
  }

  if ((chptr->mode.mode & MODE_NOCTCP) && ircd_strncmp(text, "\001ACTION ", 8)) {
    for (ch = text; *ch != '\0'; ++ch) {
      if (*ch == 1) {
        send_reply(sptr, ERR_CANNOTSENDTOCHAN, chptr->chname);
        return;
      }
    }
  }

  if (sline_check_chanmsg(sptr, chptr, text, MSG_NOTICE)) {
    return;
  }

  RevealDelayedJoinIfNeeded(sptr, chptr);
  sendcmdto_channel_butone(sptr, CMD_NOTICE, chptr, cli_from(sptr),
			   SKIP_DEAF | SKIP_BURST, "%H :%s", chptr, text);
  bot_deliver_channel(sptr, chptr, 1, text);

  if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
    sendcmdto_one(sptr, CMD_NOTICE, cli_from(sptr), "%H :%s", chptr, text);

  relay_delivered(sptr, 0, chptr, text, HOOK_MSG_NOTICE, chptr->chname);
}

/** Relay a message to a channel.
 * Generates an error if the client cannot send to the channel,
 * or if the channel is a local channel
 * @param[in] sptr Client that originated the message.
 * @param[in] name Name of target channel.
 * @param[in] text %Message to relay.
 */
void server_relay_channel_message(struct Client* sptr, const char* name, const char* text)
{
  struct Channel* chptr;
  assert(0 != sptr);
  assert(0 != name);
  assert(0 != text);

  if (IsLocalChannel(name) || 0 == (chptr = FindChannel(name))) {
    send_reply(sptr, ERR_NOSUCHCHANNEL, name);
    return;
  }
  /*
   * This first: Almost never a server/service
   * Servers may have channel services, need to check for it here
   */
  if (client_can_send_to_channel(sptr, chptr, 1) || IsChannelService(sptr)) {
    sendcmdto_channel_butone(sptr, CMD_PRIVATE, chptr, cli_from(sptr),
			     SKIP_DEAF | SKIP_BURST, "%H :%s", chptr, text);
    bot_deliver_channel(sptr, chptr, 0, text);
    relay_delivered(sptr, 0, chptr, text, HOOK_MSG_PRIVMSG, chptr->chname);
  }
  else
    send_reply(sptr, ERR_CANNOTSENDTOCHAN, chptr->chname);
}

/** Relay a notice to a channel.
 * Generates an error if the client cannot send to the channel,
 * or if the channel is a local channel
 * @param[in] sptr Client that originated the message.
 * @param[in] name Name of target channel.
 * @param[in] text %Message to relay.
 */
void server_relay_channel_notice(struct Client* sptr, const char* name, const char* text)
{
  struct Channel* chptr;
  assert(0 != sptr);
  assert(0 != name);
  assert(0 != text);

  if (IsLocalChannel(name) || 0 == (chptr = FindChannel(name)))
    return;
  /*
   * This first: Almost never a server/service
   * Servers may have channel services, need to check for it here
   */
  if (client_can_send_to_channel(sptr, chptr, 1) || IsChannelService(sptr)) {
    sendcmdto_channel_butone(sptr, CMD_NOTICE, chptr, cli_from(sptr),
			     SKIP_DEAF | SKIP_BURST, "%H :%s", chptr, text);
    bot_deliver_channel(sptr, chptr, 1, text);
    relay_delivered(sptr, 0, chptr, text, HOOK_MSG_NOTICE, chptr->chname);
  }
}

/** Relay a directed message.
 * Generates an error if the named server does not exist, if it is not
 * a services server, or if \a name names a local user and a hostmask
 * is specified but does not match.
 * @param[in] sptr Client that originated the message.
 * @param[in] name Target nickname, with optional "%hostname" suffix.
 * @param[in] server Name of target server.
 * @param[in] text %Message to relay.
 */
void relay_directed_message(struct Client* sptr, char* name, char* server, const char* text)
{
  struct Client* acptr;
  char*          host;

  assert(0 != sptr);
  assert(0 != name);
  assert(0 != text);
  assert(0 != server);

  /* A server that is not a service may still be addressed this way for
   * a service bot of its own.  Only this server can know that of itself,
   * so nick@server for another server's bots still needs that server to
   * be a service.
   */
  if ((acptr = FindServer(server + 1)) == NULL
      || (!IsService(acptr) && !(IsMe(acptr) && bot_service_count() > 0)))
  {
    send_reply(sptr, ERR_NOSUCHNICK, name);
    return;
  }
  /*
   * NICK[%host]@server addressed? See if <server> is me first
   */
  if (!IsMe(acptr))
  {
    sendcmdto_one(sptr, CMD_PRIVATE, acptr, "%s :%s", name, text);

    if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
      sendcmdto_one(sptr, CMD_PRIVATE, cli_from(sptr), "%s :%s", name, text);
    return;
  }
  /*
   * Look for an user whose NICK is equal to <name> and then
   * check if it's hostname matches <host> and if it's a local
   * user.
   */
  *server = '\0';
  if ((host = strchr(name, '%')))
    *host++ = '\0';

  /* As reported by Vampire-, it's possible to brute force finding users
   * by sending a message to each server and see which one succeeded.
   * This means we have to remove error reporting.  Sigh.  Better than
   * removing the ability to send directed messages to client servers 
   * Thanks for the suggestion Vampire=.  -- Isomer 2001-08-28
   * Argh, /ping nick@server, disallow messages to non +k clients :/  I hate
   * this. -- Isomer 2001-09-16
   */
  if (!(acptr = FindUser(name))
      || !(MyUser(acptr) || IsLocalServiceBot(acptr))
      || !IsChannelService(acptr)
      || (!EmptyString(host) && 0 != match(host, cli_user(acptr)->host)))
  {
    /*
     * By this stage we might as well not bother because they will
     * know that this server is currently linked because of the
     * increased lag.
     */
    send_reply(sptr, ERR_NOSUCHNICK, name);
    return;
  }

  if (should_block_unauth_user(sptr, acptr)) {
    send_reply_blocked_unauth_user(sptr, acptr);
    return;
  }

  if (sline_check_privmsg(sptr, acptr, text, MSG_PRIVATE)) {
    return;
  }
  
  *server = '@';
  if (host)
    *--host = '%';

  if (!(is_silenced(sptr, acptr)))
  {
    if (IsLocalServiceBot(acptr))
      bot_deliver_private(sptr, acptr, 0, text);
    else
      sendcmdto_one(sptr, CMD_PRIVATE, acptr, "%s :%s", name, text);

    if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
      sendcmdto_one(sptr, CMD_PRIVATE, cli_from(sptr), "%s :%s", name, text);
  }
}

/** Relay a directed notice.
 * Generates an error if the named server does not exist, if it is not
 * a services server, or if \a name names a local user and a hostmask
 * is specified but does not match.
 * @param[in] sptr Client that originated the message.
 * @param[in] name Target nickname, with optional "%hostname" suffix.
 * @param[in] server Name of target server.
 * @param[in] text %Message to relay.
 */
void relay_directed_notice(struct Client* sptr, char* name, char* server, const char* text)
{
  struct Client* acptr;
  char*          host;

  assert(0 != sptr);
  assert(0 != name);
  assert(0 != text);
  assert(0 != server);

  /* A server that is not a service may still be addressed this way for
   * a service bot of its own.  Only this server can know that of itself,
   * so nick@server for another server's bots still needs that server to
   * be a service.
   */
  if ((acptr = FindServer(server + 1)) == NULL
      || (!IsService(acptr) && !(IsMe(acptr) && bot_service_count() > 0)))
  {
    send_reply(sptr, ERR_NOSUCHNICK, name);
    return;
  }

  /*
   * NICK[%host]@server addressed? See if <server> is me first
   */
  if (!IsMe(acptr)) {
    sendcmdto_one(sptr, CMD_NOTICE, acptr, "%s :%s", name, text);

    if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
      sendcmdto_one(sptr, CMD_NOTICE, cli_from(sptr), "%s :%s", name, text);
    return;
  }
  /*
   * Look for an user whose NICK is equal to <name> and then
   * check if it's hostname matches <host> and if it's a local
   * user.
   */
  *server = '\0';
  if ((host = strchr(name, '%')))
    *host++ = '\0';

  if (!(acptr = FindUser(name))
      || !(MyUser(acptr) || IsLocalServiceBot(acptr))
      || (!EmptyString(host) && 0 != match(host, cli_user(acptr)->host)))
    return;

  /* Apply the same logic to NOTICE that is applied to PRIVMSG: only
   * allow services to receive /notice nick@server.undernet.org notices.
  */
  if (!IsChannelService(acptr))
  {
    send_reply(sptr, ERR_NOSUCHNICK, name);
    return;
  }

  /* No error reply: RFC 2812 forbids automatic replies to NOTICE. */
  if (should_block_unauth_user(sptr, acptr))
    return;

  if (sline_check_privmsg(sptr, acptr, text, MSG_NOTICE)) {
    return;
  }

  *server = '@';
  if (host)
    *--host = '%';

  if (!(is_silenced(sptr, acptr)))
  {
    if (IsLocalServiceBot(acptr))
      bot_deliver_private(sptr, acptr, 1, text);
    else
      sendcmdto_one(sptr, CMD_NOTICE, acptr, "%s :%s", name, text);

    if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
      sendcmdto_one(sptr, CMD_NOTICE, cli_from(sptr), "%s :%s", name, text);
  }
}

/** Check if two users share a common channel.
 * The source's zombie and delayed-join memberships are skipped: a
 * hidden join in a +D channel must not grant access to a +c member of
 * that channel.  The target's own delayed joins do count, since the
 * target knows its own channels and can see the (visible) source.
 * @param[in] sptr Source client.
 * @param[in] acptr Target client.
 * @return Non-zero if users share a channel, zero otherwise.
 */
static int has_common_channel(struct Client *sptr, struct Client *acptr)
{
  struct Membership *schan, *achan;

  for (schan = cli_user(sptr)->channel; schan; schan = schan->next_channel) {
    if (IsZombie(schan) || IsDelayedJoin(schan))
      continue;
    for (achan = cli_user(acptr)->channel; achan; achan = achan->next_channel) {
      if (IsZombie(achan))
        continue;
      if (schan->channel == achan->channel)
        return 1;  /* Found common channel */
    }
  }
  return 0;  /* No common channels */
}

/** Check whether user mode +c on \a acptr requires dropping a message
 * from \a sptr.  Servers, services (+k), IRC operators and messages
 * to self are exempt.
 * @param[in] sptr Source of the message.
 * @param[in] acptr Target of the message.
 * @return Non-zero if the message should be dropped.
 */
static int commonchans_drop(struct Client *sptr, struct Client *acptr)
{
  if (!IsCommonChans(acptr) || sptr == acptr)
    return 0;
  if (!IsUser(sptr) || IsChannelService(sptr) || IsAnOper(sptr))
    return 0;
  return !has_common_channel(sptr, acptr);
}

/** Relay a private message from a local user.
 * Returns an error if the user does not exist or sending to him would
 * exceed the source's free targets.  Sends an AWAY status message if
 * the target is marked as away.
 * @param[in] sptr Client that originated the message.
 * @param[in] name Nickname of target user.
 * @param[in] text %Message to relay.
 */
void relay_private_message(struct Client* sptr, const char* name, const char* text)
{
  struct Client* acptr;

  assert(0 != sptr);
  assert(0 != name);
  assert(0 != text);

  if (0 == (acptr = FindUser(name))) {
    send_reply(sptr, ERR_NOSUCHNICK, name);
    return;
  }

  /* Note: X does silence users who flood it. */
  if (!IsChannelService(acptr) && check_target_limit(sptr, acptr, NULL))
    return;

  if (should_block_unauth_user(sptr, acptr)) {
    send_reply_blocked_unauth_user(sptr, acptr);
    return;
  }

  if (is_silenced(sptr, acptr))
    return;

  if (sline_check_privmsg(sptr, acptr, text, MSG_PRIVATE)) {
    return;
  }

  /* Check +c mode: drop private messages from users not in a common channel */
  if (commonchans_drop(sptr, acptr))
    return;

  /*
   * send away message if user away
   */
  if (cli_user(acptr) && cli_user(acptr)->away)
    send_reply(sptr, RPL_AWAY, cli_name(acptr), cli_user(acptr)->away);
  /*
   * deliver the message
   */
  if (MyUser(acptr))
    add_target(acptr, sptr);

  if (hook_is_active(HOOK_MESSAGE_PRE_PRIVATE)) {
    struct HookContext hc;
    char rewrite[BUFSIZE];
    char alt[BUFSIZE];

    hook_context_init(&hc);
    hc.hc_client = acptr;      /* who it is going to */
    hc.hc_source = sptr;       /* who sent it */
    hc.hc_arg = text;
    hc.hc_rewrite = rewrite;
    hc.hc_rewrite_len = sizeof(rewrite);
    hc.hc_alt = alt;
    hc.hc_alt_len = sizeof(alt);
    hc.hc_alt_cap = CAP_NONE;
    rewrite[0] = '\0';
    alt[0] = '\0';

    if (hook_run(HOOK_MESSAGE_PRE_PRIVATE, &hc) == HOOK_DENY) {
      hook_deny_reply(sptr, &hc, ERR_CANNOTSENDTOCHAN, cli_name(acptr));
      return;
    }

    if (hc.hc_rewritten) {
      rewrite[sizeof(rewrite) - 1] = '\0';
      text = rewrite;
    }

    /* One recipient, so the choice is simply which body they get -- and
     * the rich one over a link, because their server makes the same
     * choice for them. */
    if (hc.hc_alt_set && hc.hc_alt_cap != CAP_NONE) {
      int theirs;

      alt[sizeof(alt) - 1] = '\0';
      theirs = MyUser(acptr) && CapHas(cli_active(acptr), hc.hc_alt_cap);

      if (IsLocalServiceBot(acptr))
        bot_deliver_private(sptr, acptr, 0, alt);
      else {
        if (!theirs && MyUser(acptr))
          msg_tag_suppress(hc.hc_alt_tag);
        sendcmdto_one(sptr, CMD_PRIVATE, acptr, "%C :%s", acptr,
                      (theirs || !MyUser(acptr)) ? text : alt);
        msg_tag_suppress(0);
      }

      if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE)) {
        int mine = CapHas(cli_active(sptr), hc.hc_alt_cap);

        if (!mine)
          msg_tag_suppress(hc.hc_alt_tag);
        sendcmdto_one(sptr, CMD_PRIVATE, cli_from(sptr), "%C :%s", acptr,
                      mine ? text : alt);
        msg_tag_suppress(0);
      }

      relay_delivered(sptr, acptr, 0, alt, HOOK_MSG_PRIVMSG,
                      cli_name(acptr));
      return;
    }
  }

  /* A service bot of this server has no connection to deliver to; the
   * module that owns it gets the message instead.
   */
  if (IsLocalServiceBot(acptr))
    bot_deliver_private(sptr, acptr, 0, text);
  else
    sendcmdto_one(sptr, CMD_PRIVATE, acptr, "%C :%s", acptr, text);

  if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
    sendcmdto_one(sptr, CMD_PRIVATE, cli_from(sptr), "%C :%s", acptr, text);

  relay_delivered(sptr, acptr, 0, text, HOOK_MSG_PRIVMSG, cli_name(acptr));
}

/** Relay a private notice from a local user.
 * Returns an error if the user does not exist or sending to him would
 * exceed the source's free targets.  Sends an AWAY status message if
 * the target is marked as away.
 * @param[in] sptr Client that originated the message.
 * @param[in] name Nickname of target user.
 * @param[in] text %Message to relay.
 */
void relay_private_notice(struct Client* sptr, const char* name, const char* text)
{
  struct Client* acptr;
  assert(0 != sptr);
  assert(0 != name);
  assert(0 != text);

  if (0 == (acptr = FindUser(name)))
    return;

  if (!IsChannelService(acptr) && check_target_limit(sptr, acptr, NULL))
    return;

  /* No error reply: RFC 2812 forbids automatic replies to NOTICE. */
  if (should_block_unauth_user(sptr, acptr))
    return;

  if (is_silenced(sptr, acptr))
    return;

  if (sline_check_privmsg(sptr, acptr, text, MSG_NOTICE)) {
    return;
  }

  /* Check +c mode: drop notices from users not in a common channel */
  if (commonchans_drop(sptr, acptr))
    return;

  /*
   * deliver the message
   */
  if (MyUser(acptr))
    add_target(acptr, sptr);

  if (IsLocalServiceBot(acptr))
    bot_deliver_private(sptr, acptr, 1, text);
  else
    sendcmdto_one(sptr, CMD_NOTICE, acptr, "%C :%s", acptr, text);

  if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
    sendcmdto_one(sptr, CMD_NOTICE, cli_from(sptr), "%C :%s", acptr, text);

  relay_delivered(sptr, acptr, 0, text, HOOK_MSG_NOTICE, cli_name(acptr));
}

/** Relay a private message that arrived from a server.
 * Returns an error if the user does not exist.
 * @param[in] sptr Client that originated the message.
 * @param[in] name Nickname of target user.
 * @param[in] text %Message to relay.
 */
void server_relay_private_message(struct Client* sptr, const char* name, const char* text)
{
  struct Client* acptr;
  assert(0 != sptr);
  assert(0 != name);
  assert(0 != text);
  /*
   * nickname addressed?
   */
  if (0 == (acptr = findNUser(name)) || !IsUser(acptr)) {
    send_reply(sptr, SND_EXPLICIT | ERR_NOSUCHNICK, N_("* :Target left %s. "
	       "Failed to deliver: [%.20s]"), feature_str(FEAT_NETWORK),
               text);
    return;
  }

  if (should_block_unauth_user(sptr, acptr)) {
    send_reply_blocked_unauth_user(sptr, acptr);
    return;
  }

  if (is_silenced(sptr, acptr))
    return;

  /* Check +c mode: drop private messages from users not in a common
   * channel.  This must be enforced here too: the origin server may
   * not do it (services packages, older servers). */
  if (commonchans_drop(sptr, acptr))
    return;

  if (MyUser(acptr))
    add_target(acptr, sptr);

  /* A service bot serves the whole network, so a message from a remote
   * user reaches its module the same way a local user's does.
   */
  if (IsLocalServiceBot(acptr))
    bot_deliver_private(sptr, acptr, 0, text);
  else
    sendcmdto_one(sptr, CMD_PRIVATE, acptr, "%C :%s", acptr, text);

  relay_delivered(sptr, acptr, 0, text, HOOK_MSG_PRIVMSG, cli_name(acptr));
}


/** Relay a private notice that arrived from a server.
 * Returns an error if the user does not exist.
 * @param[in] sptr Client that originated the message.
 * @param[in] name Nickname of target user.
 * @param[in] text %Message to relay.
 */
void server_relay_private_notice(struct Client* sptr, const char* name, const char* text)
{
  struct Client* acptr;
  assert(0 != sptr);
  assert(0 != name);
  assert(0 != text);
  /*
   * nickname addressed?
   */
  if (0 == (acptr = findNUser(name)) || !IsUser(acptr))
    return;

  /* No error reply: RFC 2812 forbids automatic replies to NOTICE. */
  if (should_block_unauth_user(sptr, acptr))
    return;

  if (is_silenced(sptr, acptr))
    return;

  /* Check +c mode: drop notices from users not in a common channel.
   * This must be enforced here too: the origin server may not do it
   * (services packages, older servers). */
  if (commonchans_drop(sptr, acptr))
    return;

  if (MyUser(acptr))
    add_target(acptr, sptr);

  if (IsLocalServiceBot(acptr))
    bot_deliver_private(sptr, acptr, 1, text);
  else
    sendcmdto_one(sptr, CMD_NOTICE, acptr, "%C :%s", acptr, text);

  relay_delivered(sptr, acptr, 0, text, HOOK_MSG_NOTICE, cli_name(acptr));
}

/** Relay a masked message from a local user.
 * Sends an error response if there is no top-level domain label in \a
 * mask, or if that TLD contains a wildcard.
 * @param[in] sptr Client that originated the message.
 * @param[in] mask Target mask for the message.
 * @param[in] text %Message to relay.
 */
void relay_masked_message(struct Client* sptr, const char* mask, const char* text)
{
  const char* s;
  int   host_mask = 0;

  assert(0 != sptr);
  assert(0 != mask);
  assert(0 != text);
  /*
   * look for the last '.' in mask and scan forward
   */
  if (0 == (s = strrchr(mask, '.'))) {
    send_reply(sptr, ERR_NOTOPLEVEL, mask);
    return;
  }
  while (*++s) {
    if (*s == '.' || *s == '*' || *s == '?')
       break;
  }
  if (*s == '*' || *s == '?') {
    send_reply(sptr, ERR_WILDTOPLEVEL, mask);
    return;
  }
  s = mask;
  if ('@' == *++s) {
    host_mask = 1;
    ++s;
  }

  sendcmdto_match_butone(sptr, CMD_PRIVATE, s,
			 IsServer(cli_from(sptr)) ? cli_from(sptr) : 0,
			 host_mask ? MATCH_HOST : MATCH_SERVER,
			 "%s :%s", mask, text);

  if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
    sendcmdto_one(sptr, CMD_PRIVATE, cli_from(sptr), "%s :%s", mask, text);

}

/* Masked ($mask) messages are oper-only in the stock configuration and are
 * intentionally not run through the S-line spam checks. */
/** Relay a masked notice from a local user.
 * Sends an error response if there is no top-level domain label in \a
 * mask, or if that TLD contains a wildcard.
 * @param[in] sptr Client that originated the message.
 * @param[in] mask Target mask for the message.
 * @param[in] text %Message to relay.
 */
void relay_masked_notice(struct Client* sptr, const char* mask, const char* text)
{
  const char* s;
  int   host_mask = 0;

  assert(0 != sptr);
  assert(0 != mask);
  assert(0 != text);
  /*
   * look for the last '.' in mask and scan forward
   */
  if (0 == (s = strrchr(mask, '.'))) {
    send_reply(sptr, ERR_NOTOPLEVEL, mask);
    return;
  }
  while (*++s) {
    if (*s == '.' || *s == '*' || *s == '?')
       break;
  }
  if (*s == '*' || *s == '?') {
    send_reply(sptr, ERR_WILDTOPLEVEL, mask);
    return;
  }
  s = mask;
  if ('@' == *++s) {
    host_mask = 1;
    ++s;
  }

  sendcmdto_match_butone(sptr, CMD_NOTICE, s,
			 IsServer(cli_from(sptr)) ? cli_from(sptr) : 0,
			 host_mask ? MATCH_HOST : MATCH_SERVER,
			 "%s :%s", mask, text);

  if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
    sendcmdto_one(sptr, CMD_NOTICE, cli_from(sptr), "%s :%s", mask, text);
}

/** Relay a masked message that arrived from a server.
 * @param[in] sptr Client that originated the message.
 * @param[in] mask Target mask for the message.
 * @param[in] text %Message to relay.
 */
void server_relay_masked_message(struct Client* sptr, const char* mask, const char* text)
{
  const char* s = mask;
  int         host_mask = 0;
  assert(0 != sptr);
  assert(0 != mask);
  assert(0 != text);

  if ('@' == *++s) {
    host_mask = 1;
    ++s;
  }
  sendcmdto_match_butone(sptr, CMD_PRIVATE, s,
			 IsServer(cli_from(sptr)) ? cli_from(sptr) : 0,
			 host_mask ? MATCH_HOST : MATCH_SERVER,
			 "%s :%s", mask, text);
}

/** Relay a masked notice that arrived from a server.
 * @param[in] sptr Client that originated the message.
 * @param[in] mask Target mask for the message.
 * @param[in] text %Message to relay.
 */
void server_relay_masked_notice(struct Client* sptr, const char* mask, const char* text)
{
  const char* s = mask;
  int         host_mask = 0;
  assert(0 != sptr);
  assert(0 != mask);
  assert(0 != text);

  if ('@' == *++s) {
    host_mask = 1;
    ++s;
  }
  sendcmdto_match_butone(sptr, CMD_NOTICE, s,
			 IsServer(cli_from(sptr)) ? cli_from(sptr) : 0,
			 host_mask ? MATCH_HOST : MATCH_SERVER,
			 "%s :%s", mask, text);
}

/** Relay a local user's TAGMSG to a channel. */
void
relay_channel_tagmsg(struct Client *sptr, const char *name)
{
  struct Channel *chptr;
  struct Membership *memb;

  assert(0 != sptr);
  assert(0 != name);

  if (0 == (chptr = FindChannel(name))) {
    send_reply(sptr, ERR_NOSUCHCHANNEL, name);
    return;
  }
  if (!client_can_send_to_channel(sptr, chptr, 0)) {
    send_reply(sptr, ERR_CANNOTSENDTOCHAN, chptr->chname);
    return;
  }
  if ((chptr->mode.mode & MODE_NOPRIVMSGS) &&
      check_target_limit(sptr, NULL, chptr))
    return;
  memb = find_member_link(chptr, sptr);
  if (memb && IsDelayedTarget(memb))
    ClearDelayedTarget(memb);

  if (sline_check_chanmsg(sptr, chptr, "", MSG_PRIVATE))
    return;

  RevealDelayedJoinIfNeeded(sptr, chptr);
  sendcmdto_channel_butone(sptr, CMD_TAGMSG, chptr, cli_from(sptr),
			   SKIP_DEAF | SKIP_BURST, "%H", chptr);

  if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
    sendcmdto_one(sptr, CMD_TAGMSG, cli_from(sptr), "%H", chptr);

  relay_delivered(sptr, 0, chptr, "", HOOK_MSG_TAGMSG, chptr->chname);
}

/** Relay a local user's TAGMSG to a user. */
void
relay_private_tagmsg(struct Client *sptr, const char *name)
{
  struct Client *acptr;

  assert(0 != sptr);
  assert(0 != name);

  if (0 == (acptr = FindUser(name))) {
    send_reply(sptr, ERR_NOSUCHNICK, name);
    return;
  }
  if ((!IsChannelService(acptr) &&
       check_target_limit(sptr, acptr, NULL)) ||
      is_silenced(sptr, acptr))
    return;

  if (sline_check_privmsg(sptr, acptr, "", MSG_PRIVATE))
    return;

  if (commonchans_drop(sptr, acptr))
    return;

  if (cli_user(acptr) && cli_user(acptr)->away)
    send_reply(sptr, RPL_AWAY, cli_name(acptr), cli_user(acptr)->away);

  if (MyUser(acptr))
    add_target(acptr, sptr);

  sendcmdto_one(sptr, CMD_TAGMSG, acptr, "%C", acptr);

  if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
    sendcmdto_one(sptr, CMD_TAGMSG, cli_from(sptr), "%C", acptr);

  relay_delivered(sptr, acptr, 0, "", HOOK_MSG_TAGMSG, cli_name(acptr));
}

/** Relay a directed TAGMSG (see relay_directed_message()). */
void
relay_directed_tagmsg(struct Client *sptr, char *name, char *server)
{
  struct Client *acptr;
  char *host;

  assert(0 != sptr);
  assert(0 != name);
  assert(0 != server);

  if ((acptr = FindServer(server + 1)) == NULL || !IsService(acptr)) {
    send_reply(sptr, ERR_NOSUCHNICK, name);
    return;
  }
  if (!IsMe(acptr)) {
    sendcmdto_one(sptr, CMD_TAGMSG, acptr, "%s", name);

    if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
      sendcmdto_one(sptr, CMD_TAGMSG, cli_from(sptr), "%s", name);
    return;
  }

  *server = '\0';
  if ((host = strchr(name, '%')))
    *host++ = '\0';

  if (!(acptr = FindUser(name)) || !MyUser(acptr) ||
      !IsChannelService(acptr) ||
      (!EmptyString(host) && 0 != match(host, cli_user(acptr)->host))) {
    send_reply(sptr, ERR_NOSUCHNICK, name);
    return;
  }

  if (sline_check_privmsg(sptr, acptr, "", MSG_PRIVATE))
    return;

  *server = '@';
  if (host)
    *--host = '%';

  if (!is_silenced(sptr, acptr)) {
    sendcmdto_one(sptr, CMD_TAGMSG, acptr, "%s", name);

    if (CapHas(cli_active(sptr), CAP_ECHOMESSAGE))
      sendcmdto_one(sptr, CMD_TAGMSG, cli_from(sptr), "%s", name);
  }
}

/** Relay a masked TAGMSG from a local oper. */
void
relay_masked_tagmsg(struct Client *sptr, const char *mask)
{
  const char *s;
  int host_mask = 0;

  assert(0 != sptr);
  assert(0 != mask);

  if (!IsOper(sptr))
    return;

  s = mask;
  if (*s == '$')
    ++s;
  if (*s == '#')
    ++s;
  if (*s == '~')
    host_mask = 1;

  sendcmdto_match_butone(sptr, CMD_TAGMSG, s,
			 IsServer(cli_from(sptr)) ? cli_from(sptr) : 0,
			 host_mask ? MATCH_HOST : MATCH_SERVER,
			 "%s", mask);
}

/** Relay a server-originated TAGMSG to a channel. */
void
server_relay_channel_tagmsg(struct Client *sptr, const char *name)
{
  struct Channel *chptr;

  assert(0 != sptr);
  assert(0 != name);

  if (IsLocalChannel(name) || 0 == (chptr = FindChannel(name))) {
    send_reply(sptr, ERR_NOSUCHCHANNEL, name);
    return;
  }
  if (client_can_send_to_channel(sptr, chptr, 1) || IsChannelService(sptr)) {
    sendcmdto_channel_butone(sptr, CMD_TAGMSG, chptr, cli_from(sptr),
			     SKIP_DEAF | SKIP_BURST, "%H", chptr);
    relay_delivered(sptr, 0, chptr, "", HOOK_MSG_TAGMSG, chptr->chname);
  } else {
    send_reply(sptr, ERR_CANNOTSENDTOCHAN, chptr->chname);
  }
}

/** Relay a server-originated TAGMSG to a user. */
void
server_relay_private_tagmsg(struct Client *sptr, const char *name)
{
  struct Client *acptr;

  assert(0 != sptr);
  assert(0 != name);

  if (0 == (acptr = findNUser(name)) || !IsUser(acptr))
    return;

  if (is_silenced(sptr, acptr))
    return;

  if (commonchans_drop(sptr, acptr))
    return;

  if (MyUser(acptr))
    add_target(acptr, sptr);

  sendcmdto_one(sptr, CMD_TAGMSG, acptr, "%C", acptr);

  relay_delivered(sptr, acptr, 0, "", HOOK_MSG_TAGMSG, cli_name(acptr));
}

/** Relay a server-originated masked TAGMSG. */
void
server_relay_masked_tagmsg(struct Client *sptr, const char *mask)
{
  const char *s;
  int host_mask = 0;

  assert(0 != sptr);
  assert(0 != mask);

  s = mask;
  if (*s == '$')
    ++s;
  if (*s == '#')
    ++s;
  if (*s == '~')
    host_mask = 1;

  sendcmdto_match_butone(sptr, CMD_TAGMSG, s,
			 IsServer(cli_from(sptr)) ? cli_from(sptr) : 0,
			 host_mask ? MATCH_HOST : MATCH_SERVER,
			 "%s", mask);
}
