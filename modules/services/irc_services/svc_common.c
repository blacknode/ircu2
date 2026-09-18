/*
 * IRC - Internet Relay Chat, modules/services/irc_services/svc_common.c
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
 * @brief Commands every service answers.
 *
 * HELP and VERSION are looked up after the service's own table, so a
 * service that wants a HELP of its own simply defines one.
 */
#include "config.h"

#include "services.h"

#include "client.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_string.h"

/** Say what a table's commands do, one line each, skipping those the
 * caller could not run anyway.
 * @param[in] call The call being answered.
 * @param[in] table Table ended by a NULL cmd_name.
 */
static void help_list(const struct ServiceCall* call,
                      const struct ServiceCommand* table)
{
  const struct ServiceCommand* c;

  for (c = table; c && c->cmd_name; c++) {
    if ((c->cmd_flags & SVC_CMD_OPER) && !IsOper(call->sc_source))
      continue;
    svc_reply(call, "  %-12s %s", c->cmd_name, _(call->sc_source, c->cmd_help));
  }
}

/** HELP [command] */
static void cmd_help(struct ServiceCall* call)
{
  const struct Service* sv = call->sc_service;

  if (call->sc_argc > 1) {
    const struct ServiceCommand* c = svc_find_command(sv->sv_type,
                                                      call->sc_argv[1]);

    if (!c || ((c->cmd_flags & SVC_CMD_OPER) && !IsOper(call->sc_source))) {
      svc_reply(call, "No help for %s.", call->sc_argv[1]);
      return;
    }

    svc_reply(call, "Syntax: %s", _(call->sc_source, c->cmd_syntax));
    svc_reply(call, "%s", _(call->sc_source, c->cmd_help));
    if (c->cmd_flags & SVC_CMD_FANTASY)
      svc_reply(call, "May also be used as %c%s on a channel %s is on.",
                SVC_FANTASY_PREFIX, c->cmd_name, sv->sv_name);
    return;
  }

  svc_reply(call, "%s - %s", sv->sv_name,
            _(call->sc_source, sv->sv_type->st_description));
  svc_reply(call, "Commands:");
  help_list(call, sv->sv_type->st_commands);
  help_list(call, svc_common_commands);
  svc_reply(call, "Type HELP <command> for the syntax of a command.");
}

/** VERSION */
static void cmd_version(struct ServiceCall* call)
{
  svc_reply(call, "%s is irc services %s on %s (%s)", call->sc_service->sv_name,
            svc_module_version(), cli_name(&me), feature_str(FEAT_NETWORK));
}

const struct ServiceCommand svc_common_commands[] = {
  { "HELP", N_("HELP [command]"), N_("List the commands, or explain one."),
    0, SVC_CMD_FANTASY, cmd_help },
  { "VERSION", "VERSION", N_("Which services these are, and where they run."),
    0, 0, cmd_version },
  { NULL, NULL, NULL, 0, 0, NULL }
};
