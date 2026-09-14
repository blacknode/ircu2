/*
 * IRC - Internet Relay Chat, modules/services/irc_services/svc_nickserv.c
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
 * @brief The "nickserv" service type: nickname services.
 *
 * This is the place for REGISTER, IDENTIFY and their kin.  What is here
 * today is the shape of a command, so that the next one is a row in the
 * table and a function above it: INFO looks a nick up on the network,
 * which needs nothing the server does not already know.
 *
 * A command that has to consult a database asks db_query() and answers
 * the user from the callback; it does not wait, because nothing on the
 * main thread may.  worker_task_set_client() is how the callback finds
 * the user again if they are still here.
 */
#include "config.h"

#include "services.h"

#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "s_user.h"

/** INFO <nick> */
static void cmd_info(struct ServiceCall* call)
{
  const char* nick = call->sc_argv[1];
  struct Client* acptr = FindUser(nick);

  if (!acptr) {
    svc_reply(call, "%s is not online.", nick);
    return;
  }

  svc_reply(call, "%s is %s@%s (%s), on %s.", cli_name(acptr),
            cli_user(acptr)->username, cli_user(acptr)->host,
            cli_info(acptr), cli_name(cli_user(acptr)->server));
  if (IsAccount(acptr))
    svc_reply(call, "%s is a registered user.", cli_name(acptr));
  if (IsServiceBot(acptr))
    svc_reply(call, "%s is a service of the network.", cli_name(acptr));
}

static const struct ServiceCommand commands[] = {
  { "INFO", N_("INFO <nick>"), N_("Show what the network knows about a nick."),
    1, 0, cmd_info },
  { NULL, NULL, NULL, 0, 0, NULL }
};

const struct ServiceType svc_type_nickserv = {
  "nickserv",
  N_("Nickname services"),
  commands
};
