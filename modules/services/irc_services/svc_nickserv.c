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
 *
 * IDENTIFY does not consult anything itself.  It hands the credential to
 * sasl_login_request(), which is where AUTHENTICATE's and ACCOUNT LOGIN's
 * go too: the question is the same one, and what is done with the answer
 * -- the nickname, the @c +r, the @c -f that comes with it -- is the same
 * as well, so there is one path from a credential to an identity and not
 * three that have to be kept in step.  What this file adds is the door a
 * frozen client can still reach: a @c PRIVMSG to a local @c +S bot is one
 * of the few things #MFLG_FROZEN_OK lets through, and it exists for
 * exactly this command.
 *
 * The grace period that makes that matter is in nick_policy.c.
 */
#include "config.h"

#include "services.h"

#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_string.h"
#include "s_user.h"
#include "sasl.h"

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

/** IDENTIFY <address> <password> [<account>] */
static void cmd_identify(struct ServiceCall* call)
{
  struct Client* sptr = call->sc_source;
  const char* account = "";
  enum SaslLogin how;

  if (!MyUser(sptr)) {
    /* The credential would have to cross the link to be checked, and
     * nothing about identifying crosses the link: every server asks the
     * same store, so the one to ask is the user's own.
     */
    svc_reply(call, "Identify with the service on your own server.");
    return;
  }

  /* The password was typed into a channel window often enough to be worth
   * saying so; it is already in that channel's log by now. */
  if (call->sc_channel) {
    svc_reply(call, "Do not send a password to a channel.  Change it.");
    return;
  }

  /* Which account, when the user did not say: the nickname they are
   * using, if that is the one they are being asked to prove -- "identify
   * to this nickname" is what a frozen client is here for, and answering
   * it by logging them in as some other account of theirs would rename
   * them out from under the question.  Otherwise the address's default,
   * which is what ACCOUNT LOGIN does.
   */
  if (call->sc_argc > 3)
    account = call->sc_argv[3];
  else if (IsFrozen(sptr))
    account = cli_name(sptr);

  if (sasl_login_request(sptr, call->sc_argv[1], account, call->sc_argv[2],
                         &how))
    return;   /* the client is gone */

  switch (how) {
  case SASL_LOGIN_ASKED:
    /* The answer arrives on its own, as a 900 or a 983.  Saying
     * "checking..." here would be one more line for every login and
     * would be wrong the moment the provider answers from its cache. */
    break;

  case SASL_LOGIN_BUSY:
    svc_reply(call, "Another login is already under way.");
    break;

  case SASL_LOGIN_ALREADY:
    svc_reply(call, "You have already authenticated.");
    break;

  case SASL_LOGIN_TOOMANY:
    svc_reply(call, "Too many failed attempts on this connection.");
    break;

  case SASL_LOGIN_NOTLS:
    svc_reply(call, "This server will not take a password over an "
                    "unencrypted connection.");
    break;

  default:
    svc_reply(call, "Nickname services are not available right now.");
    break;
  }
}

/** STATUS */
static void cmd_status(struct ServiceCall* call)
{
  struct Client* sptr = call->sc_source;

  if (IsAccount(sptr))
    svc_reply(call, "You are identified as %s.", cli_user(sptr)->account);
  else
    svc_reply(call, "You are not identified.");

  if (IsFrozen(sptr))
    svc_reply(call, "This nickname is registered and you have not proved it "
                    "is yours, so you cannot do anything else until you "
                    "identify or are renamed.");
}

static const struct ServiceCommand commands[] = {
  { "IDENTIFY", N_("IDENTIFY <address> <password> [<account>]"),
    N_("Prove an account is yours."),
    2, 0, cmd_identify },
  { "STATUS", N_("STATUS"), N_("Say whether you are identified."),
    0, 0, cmd_status },
  { "INFO", N_("INFO <nick>"), N_("Show what the network knows about a nick."),
    1, 0, cmd_info },
  { NULL, NULL, NULL, 0, 0, NULL }
};

const struct ServiceType svc_type_nickserv = {
  "nickserv",
  N_("Nickname services"),
  commands
};
