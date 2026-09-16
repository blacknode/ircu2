#ifndef INCLUDED_services_h
#define INCLUDED_services_h
/*
 * IRC - Internet Relay Chat, modules/services/irc_services/services.h
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
 * @brief What the parts of irc_services share.  Private to the module.
 *
 * The module is three layers.  irc_services.c owns the bots: it reads
 * the Service{} blocks, introduces a bot for each, brings one back when
 * the network takes it away, and turns HOOK_MESSAGE_RECEIVED into a
 * dispatch.  dispatch.c turns a line of text into a command call and
 * runs it.  The svc_*.c files are the services themselves: each defines
 * one #ServiceType, which is a name and a table of commands.
 *
 * Adding a service is adding a file: define its type with its commands
 * and list it in service_types[] in irc_services.c.  Adding a command to
 * a service is adding a row to its table.  Nothing else in the module
 * needs to know.
 */

/* Every file of the module translates with the module's own domain,
 * loaded from po/ beside the shared object; see doc/readme.translations.
 * Text in a table is marked N_() and translated where it is shown.
 */
#define I18N_DOMAIN svc_i18n
#include "ircd_i18n.h"

#include "ircd_defs.h"    /* NICKLEN */

#include <time.h>

struct Client;
struct Channel;
struct SLink;
struct ServiceType;

/** Most arguments a command line is split into, the command included. */
#define SVC_MAXARGS 16

/** Prefix that makes a channel line a command for a service on it. */
#define SVC_FANTASY_PREFIX '!'

/*
 * Command flags.
 */
/** Only IRC operators may run it. */
#define SVC_CMD_OPER     0x0001
/** May be run from a channel the service is on, as !command. */
#define SVC_CMD_FANTASY  0x0002

/** One service bot this module runs. */
struct Service {
  struct Service*           sv_next;
  const struct ServiceType* sv_type;      /**< What it is. */
  struct Client*            sv_client;    /**< The bot, or NULL while gone. */
  char                      sv_name[NICKLEN + 1];
  char*                     sv_username;  /**< From the block, or NULL. */
  char*                     sv_host;      /**< From the block, or NULL. */
  char*                     sv_description; /**< From the block, or NULL. */
  struct SLink*             sv_channels;  /**< Channels to sit on. */
  time_t                    sv_retry_at;  /**< When to try introducing again. */
  unsigned int              sv_backoff;   /**< Seconds until the next retry. */
  unsigned int              sv_stale : 1; /**< Mark for the reconcile sweep. */
  unsigned int              sv_expected : 1; /**< We are removing it ourselves. */
  unsigned int              sv_broken : 1; /**< Cannot be introduced; do not retry. */
};

/** One invocation of a command: who asked what, and where. */
struct ServiceCall {
  struct Service* sc_service;   /**< Service that was addressed. */
  struct Client*  sc_source;    /**< User who sent the line. */
  struct Channel* sc_channel;   /**< Channel, for a fantasy command; else NULL. */
  int             sc_notice;    /**< Non-zero if it arrived as a NOTICE. */
  int             sc_argc;      /**< Number of entries in sc_argv. */
  char*           sc_argv[SVC_MAXARGS]; /**< sc_argv[0] is the command, upper case. */
};

/** A command a service answers. */
struct ServiceCommand {
  const char*  cmd_name;      /**< Upper case. */
  const char*  cmd_syntax;    /**< "REGISTER <password> <email>". */
  const char*  cmd_help;      /**< One line for HELP. */
  unsigned int cmd_min_args;  /**< Arguments after the command itself. */
  unsigned int cmd_flags;     /**< SVC_CMD_* flags. */
  /** Run it.  Replies go through svc_reply(). */
  void (*cmd_run)(struct ServiceCall* call);
};

/** A kind of service: what a Service{} block's "type" names. */
struct ServiceType {
  const char* st_name;          /**< As written in the block, lower case. */
  const char* st_description;   /**< One line, for HELP. */
  /** Commands, ended by a row whose cmd_name is NULL. */
  const struct ServiceCommand* st_commands;
};

/*
 * The services.  Each svc_*.c exports its type; service_types[] in
 * irc_services.c lists them.
 */
extern const struct ServiceType svc_type_nickserv;
extern const struct ServiceType svc_type_chanserv;

/** Commands every service answers, whatever its type: HELP, VERSION. */
extern const struct ServiceCommand svc_common_commands[];

/*
 * dispatch.c
 */

/** Turn a line addressed to a service into a command call and run it.
 * @param[in] sv Service addressed.
 * @param[in] source User who sent it.
 * @param[in] chptr Channel it was said on, or NULL for a private message.
 * @param[in] notice Non-zero if it was a NOTICE.
 * @param[in] text The line; for a channel, still with the prefix.
 */
extern void svc_dispatch(struct Service* sv, struct Client* source,
                         struct Channel* chptr, int notice, const char* text);

/** Find a command of a service by name, the type's table first.
 * @param[in] type The service's type.
 * @param[in] name Command name, any case.
 * @return The command, or NULL.
 */
extern const struct ServiceCommand* svc_find_command(
  const struct ServiceType* type, const char* name);

/** Answer the user who made a call, by NOTICE from the service.
 *
 * The format is translated for the user before it is rendered, so a
 * literal passed here is a msgid: xgettext extracts it with
 * --keyword=svc_reply:2, and no _() is needed at the call site.
 * @param[in] call The call being answered.
 * @param[in] fmt printf-style format (ircd_snprintf conversions).
 */
extern void svc_reply(const struct ServiceCall* call, const char* fmt, ...);

/*
 * irc_services.c
 */

/** Version string of the module, for VERSION. */
extern const char* svc_module_version(void);

/** The module's translations, or NULL; what I18N_DOMAIN names. */
extern struct I18nDomain* svc_i18n;

/** This module's handle, for the calls that ask for one. */
extern struct ModuleHandle* svc_module(void);

/** The bot running the first service of \a type, or NULL.
 *
 * By type and not by nick: a module knows which service it implements, not
 * what an operator decided to call it.
 */
extern struct Client* svc_bot_of_type(const char* type);

/*
 * nick_policy.c -- the grace period of proposal 007 sections 5 to 7.
 */

/** Say something to \a cptr as NickServ, if this server runs one.
 *
 * What svc_reply() is for a command that answers where it stands; this is
 * for the answers that arrive later, when the call is long gone.  The
 * format is translated for \a cptr, so a literal here is a msgid.
 */
extern void nick_tell(struct Client* cptr, const char* fmt, ...);

/** Register the hooks the policy runs on.  Zero on failure. */
extern int nickpolicy_init(struct ModuleHandle* mod);

/** Re-read the Service{} options.  Start-up and every rehash. */
extern void nickpolicy_config(void);

/** Drop every hold and question.  mi_fini only. */
extern void nickpolicy_fini(void);

#endif /* INCLUDED_services_h */
