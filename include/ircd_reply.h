/*
 * IRC - Internet Relay Chat, include/ircd_reply.h
 * Copyright (C) 1990 Jarkko Oikarinen and
 *                    University of Oulu, Computing Center
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
 * @brief Interfaces for sending common replies to users.
 * @version $Id$
 */
#ifndef INCLUDED_ircd_reply_h
#define INCLUDED_ircd_reply_h

struct Client;

extern int protocol_violation(struct Client* cptr, const char* pattern, ...);
extern int need_more_params(struct Client* cptr, const char* cmd);
extern int send_reply(struct Client* to, int reply, ...);

/** Send an IRCv3 standard reply: @c FAIL, @c WARN or @c NOTE.
 *
 * The machine-readable half of an answer -- a command, a code a client can
 * switch on, an optional context, and words for a person -- for the things
 * numerics were never able to say.
 *
 * A client that did not ask for @c standard-replies cannot be sent one: it
 * would be a command it has never heard of, and the rule in this tree is
 * that a client which negotiated nothing sees what it has always seen.  It
 * gets a @c NOTICE carrying \a text instead, so the person still finds out
 * what happened, and this returns zero to say so.
 *
 * \a text is not translated here.  A module's text belongs to that
 * module's catalog, so the caller translates it -- with _() in its own
 * domain -- and passes the result.
 *
 * @param[in] to Client to answer.  Must be one of this server's.
 * @param[in] kind #MSG_FAIL, #MSG_WARN or #MSG_NOTE.
 * @param[in] command The command being answered, e.g. "CHATHISTORY", or
 *   "*" when there is no one command it belongs to.
 * @param[in] code Machine-readable code, e.g. "INVALID_PARAMS".
 * @param[in] context One extra word of context, or NULL.
 * @param[in] text Human-readable description, already translated.
 * @return Non-zero if the standard reply went out, zero if the client was
 *   sent the notice instead.
 */
extern int send_std_reply(struct Client* to, const char* kind,
                          const char* command, const char* code,
                          const char* context, const char* text);

/** send_std_reply() with #MSG_FAIL: the command did not happen. */
#define send_fail(to, command, code, context, text) \
  send_std_reply((to), MSG_FAIL, (command), (code), (context), (text))

/** send_std_reply() with #MSG_WARN: it happened, with a caveat. */
#define send_warn(to, command, code, context, text) \
  send_std_reply((to), MSG_WARN, (command), (code), (context), (text))

/** send_std_reply() with #MSG_NOTE: something worth saying, no more. */
#define send_note(to, command, code, context, text) \
  send_std_reply((to), MSG_NOTE, (command), (code), (context), (text))

#define SND_EXPLICIT	0x40000000	/**< first arg is a pattern to use */

#endif /* INCLUDED_ircd_reply_h */

