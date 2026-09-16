/*
 * IRC - Internet Relay Chat, ircd/m_account.c
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
 * @brief The ACCOUNT command: identifying without SASL.
 *
 * @verbatim
 *   ACCOUNT LOGIN <address> <password> [<account>]
 *   ACCOUNT LOGOUT
 *   ACCOUNT LIST
 * @endverbatim
 *
 * SASL is for clients that speak IRCv3; this is for every other client
 * there has ever been, and it is the same question underneath.  LOGIN
 * hands its credential to sasl_login_request(), which is where
 * AUTHENTICATE's credentials go too -- so there is one path from a
 * credential to @c +r, not two that have to be kept in step.  What the
 * command decides on its own is only how the client is spoken to.
 *
 * LOGIN works before and after registration.  Before, it behaves like
 * PASS: the answer is held with the same AR_SASL_PENDING that SASL uses,
 * the nickname is claimed as soon as it arrives and the mode is granted
 * at the end of register_user().  After, it identifies -- or switches
 * account, which is the same act.
 *
 * LIST needs an address to ask about, and the only address it will use is
 * the one in cli_user()->email, which exists only because this client
 * proved it was its own.  There is no form of this command that lists
 * somebody else's accounts: "what accounts does this address hold" asked
 * of an address the asker has not authenticated with is an enumerator,
 * and it would be one whether or not the answer were filtered afterwards.
 *
 * ACCOUNT has these three subcommands and does not grow.  Registering an
 * account, verifying an address, changing a password: none of them touch
 * @c struct @c Client, all of them are policy, and policy belongs to the
 * nickserv service (proposal 007 section 4.2).
 *
 * Like AUTHENTICATE, ACCOUNT has no P10 token.  Nothing about it crosses
 * a link: every server asks the same identity module about the same
 * store, and what travels is @c +r, which travelled already.
 */
#include "config.h"

#include "account.h"
#include "client.h"
#include "handlers.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_sha256.h"   /* ircd_crypto_wipe() */
#include "ircd_string.h"
#include "msg.h"
#include "numeric.h"
#include "s_misc.h"
#include "s_user.h"
#include "sasl.h"
#include "send.h"
#include "struct.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** Write the client's nick!user\@host into \a buf, as 900 and 901 want it.
 * @param[in] cptr Client.
 * @param[out] buf Where to write it.
 * @param[in] len Size of \a buf.
 */
static void account_mask(struct Client* cptr, char* buf, size_t len)
{
  const struct User* user = cli_user(cptr);

  ircd_snprintf(0, buf, len, "%s!%s@%s", cli_name(cptr),
                (user && *user->username) ? user->username : "*",
                (user && *user->host) ? user->host : "*");
}

/** Take the answer to an ACCOUNT LIST.
 *
 * @param[in] cptr Client that asked, or NULL if it has left.
 * @param[in] result #ACCOUNT_OK, or why there is no listing.
 * @param[in] entries The accounts; the provider's, not kept.
 * @param[in] count How many.
 * @param[in] reason Text from the provider, or NULL.
 * @param[in] data Unused.
 */
static void account_list_answer(struct Client* cptr, enum AccountResult result,
                                const struct AccountEntry* entries,
                                unsigned int count, const char* reason,
                                void* data)
{
  unsigned int i;

  (void) data;

  if (!cptr || !MyConnect(cptr) || !cli_connect(cptr))
    return;

  if (result != ACCOUNT_OK) {
    send_reply(cptr, ERR_ACCOUNTFAIL,
               reason ? reason : _(cptr, account_strerror(result)));
    return;
  }

  for (i = 0; i < count; ++i) {
    const char* nick = entries[i].ae_nick;
    int in_use;
    char flags[3];
    size_t n = 0;

    if (EmptyString(nick))
      continue;

    /* Which one is in use is the server's own knowledge, not the
     * provider's: the provider knows what the address holds, this knows
     * what the client is wearing. */
    in_use = IsAccount(cptr) && cli_user(cptr)
             && !ircd_strcmp(cli_user(cptr)->account, nick);

    if (in_use)
      flags[n++] = '*';
    if (entries[i].ae_default)
      flags[n++] = 'd';
    if (!n)
      flags[n++] = '-';
    flags[n] = '\0';

    send_reply(cptr, RPL_ACCOUNTLIST, nick, flags,
               in_use && entries[i].ae_default
                 ? _(cptr, "in use, default")
                 : in_use ? _(cptr, "in use")
                 : entries[i].ae_default ? _(cptr, "default")
                 : _(cptr, "available"));
  }

  send_reply(cptr, RPL_ENDOFACCOUNTLIST);
}

/** Handle ACCOUNT LOGIN.
 * @param[in,out] cptr Client that sent it.
 * @param[in] parc Number of parameters.
 * @param[in] parv The parameters.
 * @return Zero, or CPTR_KILLED.
 */
static int account_do_login(struct Client* cptr, int parc, char* parv[])
{
  char secret[SASL_SECRET_MAX + 1];
  enum SaslLogin how;
  int res;

  if (parc < 4 || EmptyString(parv[2]) || EmptyString(parv[3]))
    return send_reply(cptr, ERR_NEEDMOREPARAMS, "ACCOUNT LOGIN");

  if (strlen(parv[2]) > ACCOUNT_EMAIL_MAX)
    return send_reply(cptr, ERR_ACCOUNTFAIL,
                      _(cptr, "that is not an address"));

  if (strlen(parv[3]) > SASL_SECRET_MAX)
    return send_reply(cptr, ERR_ACCOUNTFAIL,
                      _(cptr, "that password is too long"));

  /* Taken out of the input buffer and wiped there before anything else
   * happens: the call below can end with the client gone, and a password
   * is not something to leave behind for whatever reuses the buffer.
   */
  ircd_strncpy(secret, parv[3], SASL_SECRET_MAX);
  ircd_crypto_wipe(parv[3], strlen(parv[3]));

  res = sasl_login_request(cptr, parv[2],
                           (parc > 4 && !EmptyString(parv[4])) ? parv[4] : "",
                           secret, &how);

  ircd_crypto_wipe(secret, sizeof(secret));

  if (res)
    return res;

  switch (how) {
  case SASL_LOGIN_ASKED:
    return 0;

  case SASL_LOGIN_BUSY:
    return send_reply(cptr, ERR_ACCOUNTFAIL,
                      _(cptr, "another login is already under way"));

  case SASL_LOGIN_ALREADY:
    return send_reply(cptr, ERR_ACCOUNTFAIL,
                      _(cptr, "you have already authenticated"));

  case SASL_LOGIN_TOOMANY:
    return send_reply(cptr, ERR_ACCOUNTFAIL,
                      _(cptr, "too many failed attempts on this connection"));

  case SASL_LOGIN_NOTLS:
    return send_reply(cptr, ERR_ACCOUNTFAIL,
                      _(cptr, "this server will not take a password over an "
                              "unencrypted connection"));

  default:
    return send_reply(cptr, ERR_ACCOUNTFAIL,
                      _(cptr, account_strerror(ACCOUNT_ERR_UNAVAILABLE)));
  }
}

/** Handle ACCOUNT LOGOUT.
 * @param[in,out] cptr Client that sent it.
 * @return Zero, or CPTR_KILLED.
 */
static int account_do_logout(struct Client* cptr)
{
  char mask[NICKLEN + USERLEN + HOSTLEN + 3];
  int res;

  if (!IsRegistered(cptr) || !IsAccount(cptr))
    return send_reply(cptr, ERR_NOTAUTHENTICATED);

  /* The nickname goes with the session, so the client has a new one by
   * the time it is told; the mask in the numeric is what it is now, not
   * what it was. */
  res = account_logout(cptr);

  if (res == CPTR_KILLED)
    return res;

  if (!res)
    return send_reply(cptr, ERR_NOTAUTHENTICATED);

  account_mask(cptr, mask, sizeof(mask));

  return send_reply(cptr, RPL_LOGGEDOUT, mask);
}

/** Handle ACCOUNT LIST.
 * @param[in,out] cptr Client that sent it.
 * @return Zero.
 */
static int account_do_list(struct Client* cptr)
{
  const struct User* user = cli_user(cptr);

  /* The address, not the mode, is what this needs: it is the question's
   * only input, and it is there only because the client authenticated. */
  if (!IsRegistered(cptr) || !user || EmptyString(user->email))
    return send_reply(cptr, ERR_NOTAUTHENTICATED);

  if (!account_list(cptr, user->email, account_list_answer, 0))
    return send_reply(cptr, ERR_ACCOUNTFAIL,
                      _(cptr, account_strerror(ACCOUNT_ERR_UNAVAILABLE)));

  return 0;
}

/** Handle ACCOUNT from a client, registered or not.
 *
 * @param[in] cptr Client that sent the line.
 * @param[in] sptr Source; the same client.
 * @param[in] parc Number of parameters.
 * @param[in] parv The parameters.
 * @return Zero, or CPTR_KILLED.
 */
int m_account(struct Client* cptr, struct Client* sptr, int parc, char* parv[])
{
  char sub[32];

  assert(0 != cptr);
  assert(cptr == sptr);

  if (parc < 2 || EmptyString(parv[1]))
    return send_reply(cptr, ERR_NEEDMOREPARAMS, "ACCOUNT");

  if (!ircd_strcmp(parv[1], "LOGIN"))
    return account_do_login(cptr, parc, parv);

  if (!ircd_strcmp(parv[1], "LOGOUT"))
    return account_do_logout(cptr);

  if (!ircd_strcmp(parv[1], "LIST"))
    return account_do_list(cptr);

  /* 421 rather than a failed authentication: nothing was attempted, and
   * the three subcommands are the whole command. */
  ircd_snprintf(0, sub, sizeof(sub), "ACCOUNT %s", parv[1]);

  return send_reply(cptr, ERR_UNKNOWNCOMMAND, sub);
}
