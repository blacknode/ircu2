/*
 * IRC - Internet Relay Chat, modules/services/irc_services/svc_chanserv.c
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
 * @brief The "chanserv" service type: channel services.
 *
 * The place for REGISTER, OP, ACCESS and the rest.  INFO is here to show
 * the two ways a command reaches a channel service: as a private
 * "INFO #channel", and as "!INFO" said on a channel the service is on,
 * where the channel is the one the line was said on.
 */
#include "config.h"

#include "services.h"

#include "channel.h"
#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "s_misc.h"

/** The channel a call is about: the argument, or the channel it was said
 * on.  Answers the user when there is none.
 * @param[in] call The call.
 * @param[in] arg Index in sc_argv of the channel argument.
 * @return The channel, or NULL after a reply.
 */
static struct Channel* call_channel(struct ServiceCall* call, int arg)
{
  struct Channel* chptr;
  const char* name;

  if (call->sc_channel)
    return call->sc_channel;

  if (call->sc_argc <= arg) {
    svc_reply(call, "Which channel?  Syntax: %s",
              _(call->sc_source,
                svc_find_command(call->sc_service->sv_type,
                                 call->sc_argv[0])->cmd_syntax));
    return NULL;
  }

  name = call->sc_argv[arg];
  chptr = FindChannel(name);

  /* What /LIST would not show the user, this does not either. */
  if (!chptr || !ShowChannel(call->sc_source, chptr)) {
    svc_reply(call, "Channel %s does not exist.", name);
    return NULL;
  }

  return chptr;
}

/** INFO [#channel] */
static void cmd_info(struct ServiceCall* call)
{
  struct Channel* chptr = call_channel(call, 1);

  if (!chptr)
    return;

  svc_reply(call, _n(call->sc_source,
                     "%s has %u user and was created %s.",
                     "%s has %u users and was created %s.", chptr->users),
            chptr->chname, chptr->users, myctime(chptr->creationtime));
  if (chptr->topic[0])
    svc_reply(call, "Topic: %s (set by %s)", chptr->topic, chptr->topic_nick);
}

static const struct ServiceCommand commands[] = {
  { "INFO", N_("INFO <#channel>"),
    N_("Show what the network knows about a channel."),
    0, SVC_CMD_FANTASY, cmd_info },
  { NULL, NULL, NULL, 0, 0, NULL }
};

const struct ServiceType svc_type_chanserv = {
  "chanserv",
  N_("Channel services"),
  commands
};
