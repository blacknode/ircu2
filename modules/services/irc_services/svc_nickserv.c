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

#include "account.h"
#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_string.h"
#include "numnicks.h"
#include "s_conf.h"
#include "struct.h"

#include <string.h>
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

/*
 * The writes.  All three go through account_change(), which is the core's
 * one entry point to the store; the identity module does the locking and
 * the Argon2, and the answer comes back here long after the command that
 * asked for it -- through nick_tell(), because there is no ServiceCall
 * left by then.
 */

/** Accounts one address may hold, when the block does not say. */
#define NICKSERV_MAX_ACCOUNTS 3

/** What an answer is about, packed into the callback's opaque pointer.
 *
 * A small integer rather than something allocated: a request whose client
 * leaves is dropped without its callback running, so anything allocated
 * here would be leaked by exactly the case that is hardest to notice.
 */
enum NickWrite {
  NICKW_REGISTER = 1,
  NICKW_PASSWORD,
  NICKW_DROP
};

/** Take the answer to a write.
 * @param[in] sptr Client that asked, or NULL if it has left.
 * @param[in] result What happened.
 * @param[in] nick The account, on success.
 * @param[in] email Its address, on success.
 * @param[in] reason Text from the provider, or NULL.
 * @param[in] data The #NickWrite.
 */
static void nick_write_done(struct Client* sptr, enum AccountResult result,
                            const char* nick, const char* email,
                            const char* reason, void* data)
{
  enum NickWrite what = (enum NickWrite) (size_t) data;

  if (!sptr || !MyUser(sptr))
    return;

  if (result != ACCOUNT_OK) {
    nick_tell(sptr, "That did not work: %s",
              reason ? reason : _(sptr, account_strerror(result)));
    return;
  }

  switch (what) {
  case NICKW_REGISTER:
    nick_tell(sptr, "%s is yours.", nick ? nick : "");

    /* And logged in on the spot.  The password was just checked -- or
     * just set -- so asking for it again to grant what it has already
     * earned would be ceremony.  This is the same act account_login()
     * performs for SASL, and the only one that grants +r. */
    if (nick && !account_login(sptr, nick, email))
      nick_tell(sptr, "Registered, but the nickname could not be taken; "
                      "identify when you can.");
    break;

  case NICKW_PASSWORD:
    nick_tell(sptr, "Your password has been changed.");
    break;

  default:
    nick_tell(sptr, "%s is no longer registered.", nick ? nick : "");

    /* The account is gone, so the +r that said it was yours has to go
     * with it; account_logout() takes the nickname back at the same
     * time, which is the state the model does define. */
    account_logout(sptr);
    break;
  }
}

/** Fill in what every write carries and send it.
 * @param[in] call The call being answered.
 * @param[in] req The change; ach_ip and ach_tls are filled in here.
 * @param[in] what What the answer will be about.
 */
static void nick_write(struct ServiceCall* call, struct AccountChange* req,
                       enum NickWrite what)
{
  struct Client* sptr = call->sc_source;

  req->ach_ip = ircd_ntoa(&cli_ip(sptr));
  req->ach_tls = IsTLS(sptr) ? 1 : 0;

  if (!account_change(sptr, req, nick_write_done, (void*) (size_t) what))
    svc_reply(call, "Nickname services are not available right now.");
}

/** Non-zero if \a call may carry a password at all.
 *
 * The same two refusals every one of these makes: a password typed into a
 * channel is already in that channel's log, and a credential checked on
 * another server is a credential that crossed the link, which nothing
 * here does.
 */
static int nick_write_ok(struct ServiceCall* call)
{
  if (!MyUser(call->sc_source)) {
    svc_reply(call, "Use the service on your own server.");
    return 0;
  }

  if (call->sc_channel) {
    svc_reply(call, "Do not send a password to a channel.  Change it.");
    return 0;
  }

  if (!IsTLS(call->sc_source) && feature_bool(FEAT_ACCOUNT_REQUIRE_TLS)) {
    svc_reply(call, "This server will not take a password over an "
                    "unencrypted connection.");
    return 0;
  }

  return 1;
}

/** REGISTER <address> <password> */
static void cmd_register(struct ServiceCall* call)
{
  struct Client* sptr = call->sc_source;
  struct AccountChange req;

  if (!nick_write_ok(call))
    return;

  if (IsAccount(sptr)) {
    svc_reply(call, "You are identified as %s.  Change nickname to the one "
              "you want to register first.", cli_user(sptr)->account);
    return;
  }

  memset(&req, 0, sizeof(req));
  req.ach_what = ACCOUNT_WRITE_REGISTER;
  req.ach_email = call->sc_argv[1];
  req.ach_secret = call->sc_argv[2];
  req.ach_secretlen = strlen(call->sc_argv[2]);

  /* The nickname in use, and no other: an account is a nickname, so
   * registering one you are not wearing would be registering a name you
   * have not shown you can hold. */
  req.ach_nick = cli_name(sptr);
  req.ach_max = conf_service_option_int(conf_find_service_type("nickserv"),
                                        "max_accounts",
                                        NICKSERV_MAX_ACCOUNTS);

  nick_write(call, &req, NICKW_REGISTER);
}

/** PASSWORD <old> <new> */
static void cmd_password(struct ServiceCall* call)
{
  struct Client* sptr = call->sc_source;
  struct AccountChange req;

  if (!nick_write_ok(call))
    return;

  /* The address is the one this client proved was its own; there is no
   * form of this command that changes somebody else's password. */
  if (!IsAccount(sptr) || !cli_user(sptr) || EmptyString(cli_user(sptr)->email)) {
    svc_reply(call, "Identify first.");
    return;
  }

  memset(&req, 0, sizeof(req));
  req.ach_what = ACCOUNT_WRITE_PASSWD;
  req.ach_email = cli_user(sptr)->email;
  req.ach_secret = call->sc_argv[1];
  req.ach_secretlen = strlen(call->sc_argv[1]);
  req.ach_new = call->sc_argv[2];
  req.ach_newlen = strlen(call->sc_argv[2]);

  nick_write(call, &req, NICKW_PASSWORD);
}

/** DROP <password> */
static void cmd_drop(struct ServiceCall* call)
{
  struct Client* sptr = call->sc_source;
  struct AccountChange req;

  if (!nick_write_ok(call))
    return;

  if (!IsAccount(sptr) || !cli_user(sptr) || EmptyString(cli_user(sptr)->email)) {
    svc_reply(call, "Identify first.");
    return;
  }

  memset(&req, 0, sizeof(req));
  req.ach_what = ACCOUNT_WRITE_DROP;
  req.ach_email = cli_user(sptr)->email;
  req.ach_secret = call->sc_argv[1];
  req.ach_secretlen = strlen(call->sc_argv[1]);

  /* The one in use, which is the one this client is identified to. */
  req.ach_nick = cli_name(sptr);

  nick_write(call, &req, NICKW_DROP);
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
  { "REGISTER", N_("REGISTER <address> <password>"),
    N_("Register the nickname you are using."),
    2, 0, cmd_register },
  { "PASSWORD", N_("PASSWORD <old> <new>"),
    N_("Change the password of the address you identified with."),
    2, 0, cmd_password },
  { "DROP", N_("DROP <password>"),
    N_("Give up the nickname you are identified to."),
    1, 0, cmd_drop },
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
