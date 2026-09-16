/*
 * IRC - Internet Relay Chat, modules/services/history/history.h
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
 * @brief Private declarations for the history module.
 *
 * Not installed and not visible to anything else.
 */
#ifndef INCLUDED_history_h
#define INCLUDED_history_h

#include "hist_store.h"

struct ModuleHandle;
struct HookContext;
struct Client;

/** The command this module registers.
 *
 * Here and not in include/msg.h because a module's command is not the
 * core's: the name and the token are what module_add_command() is given,
 * and nothing in the core has to know either.  There is no P10 token
 * worth the name because no server sends this to another -- a client asks
 * its own server, which reads its own store.
 */
#define MSG_CHATHISTORY "CHATHISTORY"
#define TOK_CHATHISTORY "CHATHISTORY"

/** The capability that advertises it, and its limit as the value. */
#define HIST_CAP_NAME "draft/chathistory"

/** The domain this module's own text is translated in. */
#define I18N_DOMAIN hist_i18n

/** Opened before mi_init and closed after mi_fini; see readme.translations. */
extern struct I18nDomain* hist_i18n;

/** This module's handle; db_exec() wants it. */
extern struct ModuleHandle* hist_mod;

/*
 * Capture (hist_capture.c).
 */

/** What #HOOK_MESSAGE_DELIVERED is attached to. */
extern enum HookResult hist_capture(struct HookContext* ctx, void* user);

/** Lower-case \a name the way IRC does, into \a buf.
 *
 * ToLower() and not tolower(): '[', ']' and '\\' are the upper case of
 * '{', '}' and '|' in a nickname and in a channel name, so a store that
 * used the C library's idea of case would file @c #Foo and @c #foo
 * together but @c #Foo[1] and @c #foo{1} apart.
 */
extern void hist_canon(char* buf, size_t buflen, const char* name);

/*
 * Reading (hist_read.c).
 */

/** CHATHISTORY, from a client. */
extern int hist_m_chathistory(struct Client* cptr, struct Client* sptr,
                              int parc, char* parv[]);

#endif /* INCLUDED_history_h */
