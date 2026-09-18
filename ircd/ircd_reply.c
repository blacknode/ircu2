/*
 * IRC - Internet Relay Chat, ircd/m_proto.c
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
 * @brief Implementation of functions to send common replies to users.
 * @version $Id$
 */
#include "config.h"

#include "ircd_reply.h"
#include "client.h"
#include "capab.h"
#include "ircd.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "msg.h"
#include "msgq.h"
#include "numeric.h"
#include "s_conf.h"
#include "s_debug.h"
#include "send.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** Report a protocol violation warning to anyone listening.  This can
 * be easily used to clean up the last couple of parts of the code.
 * @param[in] cptr Client that violated the protocol.
 * @param[in] pattern Description of how the protocol was violated.
 * @return Zero.
 */
int protocol_violation(struct Client* cptr, const char* pattern, ...)
{
  struct VarData vd;
  char message[BUFSIZE];

  assert(pattern);
  assert(cptr);

  vd.vd_format = pattern;
  va_start(vd.vd_args, pattern);
  ircd_snprintf(NULL, message, sizeof(message),
                "Protocol Violation from %s: %v", cli_name(cptr), &vd);
  va_end(vd.vd_args);

  sendwallto_group_butone(&me, WALL_DESYNCH, NULL, "%s", message);
  return 0;
}

/** Inform a client that they need to provide more parameters.
 * @param[in] cptr Taciturn client.
 * @param[in] cmd Command name.
 * @return Zero.
 */
int need_more_params(struct Client* cptr, const char* cmd)
{
  send_reply(cptr, ERR_NEEDMOREPARAMS, cmd);
  return 0;
}

/** Send a generic reply to a user.
 *
 * This is the one place that knows the numeric, the format and the
 * recipient at once, so it is where the format is translated: a numeric's
 * own format is looked up in the core domain with the numeric's code as
 * context, an explicit one (SND_EXPLICIT) with no context.  A client with
 * no language preference on a server with no DEFAULT_LANGUAGE pays one
 * comparison for this; see ircd_i18n.h.
 * @param[in] to Client that wants a reply.
 * @param[in] reply Numeric of message to send.
 * @return Zero.
 */
int send_reply(struct Client *to, int reply, ...)
{
  struct VarData vd;
  struct MsgBuf *mb;
  const struct Numeric *num;

  assert(0 != to);
  assert(0 != reply);

  num = get_error_numeric(reply & ~SND_EXPLICIT); /* get reply... */

  va_start(vd.vd_args, reply);

  if (reply & SND_EXPLICIT) /* get right pattern */
    vd.vd_format = i18n_text(i18n_core, to,
                             (const char *) va_arg(vd.vd_args, char *));
  else
    vd.vd_format = i18n_ctext(i18n_core, to, num->str, num->format);

  assert(0 != vd.vd_format);

  /* build buffer */
  mb = msgq_make(cli_from(to), "%:#C %s %C %v", &me, num->str, to, &vd);

  va_end(vd.vd_args);

  /* send it to the user */
  send_buffer(to, NULL, mb, 0, NULL, NULL);

  msgq_clean(mb);

  return 0; /* convenience return */
}




/** Send an IRCv3 standard reply, or the notice that stands in for one.
 * @param[in] to Client to answer.
 * @param[in] kind MSG_FAIL, MSG_WARN or MSG_NOTE.
 * @param[in] command The command being answered.
 * @param[in] code Machine-readable code.
 * @param[in] context One extra word of context, or NULL.
 * @param[in] text Human-readable description, already translated.
 * @return Non-zero if the standard reply went out.
 */
int send_std_reply(struct Client* to, const char* kind, const char* command,
                   const char* code, const char* context, const char* text)
{
  assert(0 != to);
  assert(0 != kind);
  assert(0 != command);
  assert(0 != code);

  if (!text)
    text = "";

  /* A client that never asked for standard-replies has never heard of
   * FAIL, and sending it one would be a line it cannot parse.  It gets the
   * words, which is the half of this a person needed anyway.
   */
  if (!MyConnect(to) || !CapActive(to, CAP_STANDARDREPLIES)) {
    sendcmdto_one(&me, CMD_NOTICE, to, "%C :%s", to, text);
    return 0;
  }

  /* The name is the token too: no server sends one of these, so there is
   * nothing to agree on and nothing to claim in the P10 table. */
  if (context && *context)
    sendcmdto_one(&me, kind, kind, to, "%s %s %s :%s", command, code,
                  context, text);
  else
    sendcmdto_one(&me, kind, kind, to, "%s %s :%s", command, code, text);

  return 1;
}
