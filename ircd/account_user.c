/*
 * IRC - Internet Relay Chat, ircd/account_user.c
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
 * @brief What an authentication does to a user.
 *
 * The other half of the account subsystem.  ircd/account.c asks the
 * provider and holds the questions; this applies the answers, which needs
 * the client list, the hash tables, the nick machinery and the send layer.
 * Keeping them apart is what lets the register be tested without a server,
 * the same split as migration.c against migration_run.c.
 */
#include "config.h"

#include "account.h"
#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "msg.h"
#include "numeric.h"
#include "s_debug.h"
#include "s_misc.h"
#include "s_user.h"
#include "send.h"
#include "struct.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** Rename a local client, whatever it was called before.
 *
 * set_nick_name() is the one place that knows how a rename is done -- the
 * hash tables, the echo to the client, the propagation, +r coming off --
 * so this goes through it rather than growing a second copy of that
 * knowledge.  What it has to undo first is the anti-flood delay: that
 * exists to stop a user changing nick in a loop, and the server renaming
 * somebody is not the user doing anything.
 *
 * @param[in,out] cptr Client to rename.
 * @param[in] nick The new nickname.
 * @return Zero if the client is still here, CPTR_KILLED if it is not.
 */
static int account_rename(struct Client* cptr, const char* nick)
{
  char* parv[3];
  char nickbuf[NICKLEN + 2];

  ircd_strncpy(nickbuf, nick, NICKLEN + 1);

  parv[0] = cli_name(cptr);
  parv[1] = nickbuf;
  parv[2] = NULL;

  cli_nextnick(cptr) = CurrentTime;

  return set_nick_name(cptr, cptr, nickbuf, 2, parv);
}

/** Rename \a cptr to a guest nickname, or kill it if that name is taken.
 * @param[in,out] cptr Client to rename.
 * @param[in] reason Why, for the KILL if it comes to that.
 * @return Zero if the client is still here, CPTR_KILLED if it is not.
 */
int account_force_guest(struct Client* cptr, const char* reason)
{
  char nick[NICKLEN + 2];

  assert(0 != cptr);
  assert(MyConnect(cptr));

  if (!account_guest_nick(nick, sizeof(nick))) {
    /* GUEST_PREFIX leaves no room for the random part, which would make
     * every guest the same guest.  Refusing the connection is bad; giving
     * two strangers one nickname is worse.
     */
    log_write(LS_SYSTEM, L_ERROR, 0,
              "GUEST_PREFIX \"%s\" leaves no room for a guest nickname",
              feature_str(FEAT_GUEST_PREFIX));
    return exit_client(cptr, cptr, &me,
                       reason ? reason : "Cannot assign a guest nickname");
  }

  /* Eight random base-62 characters: a name that is already taken is not
   * something to retry around, it is something that does not happen.  If
   * it does, the client goes rather than being left with a nickname the
   * server could not give it.
   */
  if (FindClient(nick)) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Guest nickname %s was already taken; killing %s", nick,
              cli_name(cptr));
    return exit_client(cptr, cptr, &me,
                       reason ? reason : "Cannot assign a guest nickname");
  }

  return account_rename(cptr, nick);
}

/** Take the nickname an account owns, without granting +r.
 * @param[in,out] cptr Client.
 * @param[in] nick The account.
 * @return Non-zero if the client now has that nickname.
 */
int account_claim_nick(struct Client* cptr, const char* nick)
{
  struct Client* acptr;

  assert(0 != cptr);
  assert(MyConnect(cptr));

  if (EmptyString(nick))
    return 0;

  if (0 == ircd_strcmp(cli_name(cptr), nick))
    return 1;

  acptr = FindClient(nick);
  if (acptr && acptr != cptr)
    return 0;

  return account_rename(cptr, nick) != CPTR_KILLED;
}

/** Grant \a cptr the account \a nick, renaming it if it is not called that.
 * @param[in,out] cptr Client that authenticated.
 * @param[in] nick The account.
 * @param[in] email Address of the identity, or NULL.
 * @return Non-zero on success.
 */
int account_login(struct Client* cptr, const char* nick, const char* email)
{
  struct Client* acptr;

  assert(0 != cptr);
  assert(MyConnect(cptr));

  if (EmptyString(nick))
    return 0;

  /* Somebody else is wearing it.  Nothing is changed: half an
   * authentication -- identified to an account whose nickname is on
   * another client -- is the one state this model does not have.
   */
  acptr = FindClient(nick);
  if (acptr && acptr != cptr)
    return 0;

  if (0 != ircd_strcmp(cli_name(cptr), nick)) {
    if (account_rename(cptr, nick) == CPTR_KILLED)
      return 0;
  }

  /* The address goes on before +r, because do_user_mode() clears it when
   * +r comes off and a reader should never see one without the other.
   */
  user_set_email(cptr, email);

  /* +r through the server's own hand: the account name follows the flag
   * in do_user_mode(), and it takes the nickname in use, which is the one
   * this client now has.
   */
  {
    char* parv[4];
    char modebuf[8];

    strcpy(modebuf, "+r");
    parv[0] = cli_name(&me);
    parv[1] = cli_name(cptr);
    parv[2] = modebuf;
    parv[3] = NULL;

    set_user_mode_on(&me, &me, cptr, 3, parv);
  }

  if (!IsAccount(cptr)) {
    /* The mode was refused, which cannot happen from &me and would be a
     * bug here rather than a path a caller can reach.  What must not
     * happen either way is the client keeping the account's nickname
     * without the flag that says it earned it, so it goes back to being
     * a guest rather than being left halfway.
     */
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Could not grant +r to %s for account %s", cli_name(cptr),
              nick);
    user_clear_email(cptr);
    account_force_guest(cptr, "Authentication failed");
    return 0;
  }

  return 1;
}

/** Log \a cptr out and rename it to a guest.
 * @param[in,out] cptr Client to log out.
 * @return Non-zero if it was logged in.
 */
int account_logout(struct Client* cptr)
{
  char* parv[4];
  char modebuf[8];

  assert(0 != cptr);
  assert(MyConnect(cptr));

  if (!IsAccount(cptr))
    return 0;

  strcpy(modebuf, "-r");
  parv[0] = cli_name(&me);
  parv[1] = cli_name(cptr);
  parv[2] = modebuf;
  parv[3] = NULL;

  /* This clears the address too: do_user_mode() lets the two go
   * together, which is the only way they are ever meant to move.
   */
  set_user_mode_on(&me, &me, cptr, 3, parv);

  /* And the nickname goes back with the session.  A client still called
   * "maria" but no longer +r is exactly what an onlooker cannot tell
   * apart from an impostor; see proposal 007 section 5.
   */
  if (account_force_guest(cptr, "Logged out"))
    return CPTR_KILLED;

  return 1;
}

/** Forget everything about a client that is leaving. */
void account_client_exiting(struct Client* cptr)
{
  account_cancel_client(cptr);
}
