/*
 * IRC - Internet Relay Chat, ircd/m_authenticate.c
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
 * @brief The AUTHENTICATE command: IRCv3 SASL on the wire.
 *
 * Three files meet here and none of them knows about the other two.
 * ircd/sasl.c turns AUTHENTICATE lines into a credential and knows nothing
 * about clients; ircd/account.c asks a module whether the credential is
 * good and knows nothing about the protocol; ircd/account_user.c grants
 * @c +r.  This is the wiring, and the only place that holds all three at
 * once.
 *
 * It is also where the two ways in differ.  A client that authenticates
 * after registering is simply logged in.  One that authenticates during
 * registration cannot be: a client that is not a user yet cannot carry a
 * user mode, so the answer is held until register_user() has run, and
 * only the nickname is taken beforehand -- so the client is introduced to
 * the network under the name it is entitled to rather than being renamed
 * a moment later.
 *
 * AUTHENTICATE never crosses P10.  Every server runs the identity module
 * against the same store, so there is nothing to route and no half-open
 * session to keep on the far side of a netsplit; what travels is @c +r,
 * which travelled already.  See proposal 007 section 2.
 */
#include "config.h"

#include "account.h"
#include "capab.h"
#include "client.h"
#include "handlers.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_base64.h"
#include "ircd_features.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "msg.h"
#include "numeric.h"
#include "s_auth.h"
#include "s_debug.h"
#include "s_misc.h"
#include "s_user.h"
#include "sasl.h"
#include "send.h"
#include "struct.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** One connection's SASL exchange.
 *
 * Kept off @c struct @c Connection by a pointer, because most connections
 * never authenticate and this is two and a half kilobytes of password
 * buffer that they would carry for nothing.
 */
struct SaslState {
  struct SaslSession ss_session;   /**< The exchange itself. */
  account_id_t       ss_query;     /**< Question in flight, or 0. */
  int                ss_attempts;  /**< Exchanges started on this link. */
  int                ss_done;      /**< An attempt has succeeded. */
  /** Held until registration finishes, for a client that authenticated
   * before it was a user.  A user mode cannot be granted to something
   * that is not a user yet.
   */
  int                ss_deferred;
  /** The credential in flight came from ACCOUNT, not AUTHENTICATE.
   *
   * The same question, asked by a different command, and the client is
   * answered in the terms of the one it used: a client that never
   * negotiated the sasl capability must not be told that "SASL
   * authentication" succeeded or failed.
   */
  int                ss_from_account;
  char               ss_account[NICKLEN + 1];
  char               ss_email[ACCOUNT_EMAIL_MAX + 1];
};

/** Most failures in a row one connection may make.
 *
 * Failures, not attempts: a successful login resets the count, because an
 * account switch is a login and a client that has just proved who it is
 * has not attacked anything.  Each failure costs a database query, and a
 * connection that has failed this many is not about to succeed.
 */
#define SASL_MAX_ATTEMPTS 3

/** Set while sasl_answer() runs, so that the call which asked can tell a
 * provider that answered from inside account_verify() -- one with a cache
 * will -- from one that will answer later.  It matters because applying an
 * answer can exit the client, which frees both the client and the state
 * that asked: after a synchronous answer there is nothing left to read.
 */
static int sasl_answer_ran;

/** What applying that answer returned: zero, or #CPTR_KILLED. */
static int sasl_answer_killed;

/** Recompute what the "sasl" capability advertises, and whether at all. */
void sasl_advertise(void)
{
  const struct Capability* cap = cap_find_index(CAP_SASL);
  char mechs[CAPVALUELEN];

  if (!cap)
    return;

  sasl_mechanisms_str(mechs, sizeof(mechs));
  cap_set_value(CAP_SASL, mechs);

  /* Offered only when somebody can answer.  A client that negotiates SASL
   * against a server with no identity provider gets as far as sending its
   * password before finding out, which is worse than seeing that the
   * capability is not there.
   */
  cap_update_availability(CAP_SASL, account_have_provider() && *mechs);
}

/** The exchange for \a cptr, created if it has none. */
static struct SaslState* sasl_state(struct Client* cptr)
{
  struct SaslState* st = cli_sasl(cptr);

  if (!st) {
    st = (struct SaslState*) MyCalloc(1, sizeof(struct SaslState));
    sasl_session_init(&st->ss_session);
    cli_sasl(cptr) = st;
  }

  return st;
}

/** Forget a client's exchange.  Called when the connection goes.
 * @param[in] cptr Client that is leaving.
 */
void sasl_client_exiting(struct Client* cptr)
{
  struct SaslState* st;

  if (!MyConnect(cptr) || !cli_connect(cptr))
    return;

  if (!(st = cli_sasl(cptr)))
    return;

  cli_sasl(cptr) = NULL;

  /* The session holds a password.  Wiping is what sasl_session_clear()
   * is for, and it runs on every way out including this one.
   */
  sasl_session_clear(&st->ss_session);
  MyFree(st);
}

/** Non-zero while \a cptr is in the middle of a SASL exchange.
 * @param[in] cptr Client to test.
 */
int sasl_in_progress(struct Client* cptr)
{
  struct SaslState* st;

  if (!MyConnect(cptr) || !cli_connect(cptr))
    return 0;

  if (!(st = cli_sasl(cptr)))
    return 0;

  /* A mechanism is chosen and nothing has been asked of the provider yet:
   * what comes next is a continuation of the credential.
   */
  return st->ss_session.ss_mech != NULL && !st->ss_query && !st->ss_done;
}

/** End an attempt and tell the client, releasing the registration hold.
 * @param[in] cptr Client.
 * @param[in] numeric What to send, or 0 to send nothing.
 * @return Zero, or CPTR_KILLED.
 */
static int sasl_fail(struct Client* cptr, int numeric)
{
  struct SaslState* st = cli_sasl(cptr);

  if (st) {
    st->ss_query = 0;
    st->ss_from_account = 0;
    sasl_session_clear(&st->ss_session);
  }

  if (numeric)
    send_reply(cptr, numeric);

  /* Registration was waiting on the answer; failing is an answer.  The
   * client is let in without +r, under whatever nickname it is entitled
   * to -- which, if it asked for a registered one, is not the one it
   * asked for; that is the identity module's business, through the
   * grace period in proposal 007 section 6.
   */
  if (!IsRegistered(cptr) && cli_auth(cptr))
    return auth_sasl_done(cli_auth(cptr));

  return 0;
}

/** Tell a client it is logged in, and close the exchange.
 *
 * Sent as soon as the answer arrives, before registration finishes rather
 * than after.  A client that authenticates during CAP negotiation waits
 * for the 903 before it sends CAP END, and CAP END is what lets
 * registration finish: holding the numeric until after registration would
 * be each side waiting for the other.
 *
 * @param[in] cptr Client.
 * @param[in,out] st Its exchange.
 */
static void sasl_tell_logged_in(struct Client* cptr, struct SaslState* st)
{
  const struct User* user = cli_user(cptr);
  char mask[NICKLEN + USERLEN + HOSTLEN + 3];

  /* During registration the username and the host may not be settled; the
   * account is what the client came for, and it is what both the mask and
   * the text end with. */
  ircd_snprintf(0, mask, sizeof(mask), "%s!%s@%s", cli_name(cptr),
                (user && *user->username) ? user->username : "*",
                (user && *user->host) ? user->host : "*");

  send_reply(cptr, RPL_LOGGEDIN, mask, st->ss_account, st->ss_account);

  /* 903 says SASL, and means it: a client that logged in with ACCOUNT
   * never asked for the capability and is not expecting the numeric. */
  if (!st->ss_from_account)
    send_reply(cptr, RPL_SASLSUCCESS);

  st->ss_done = 1;
  st->ss_query = 0;
  st->ss_attempts = 0;
  st->ss_from_account = 0;
  sasl_session_clear(&st->ss_session);
}

/** Report a failed attempt in the terms of the command that made it.
 * @param[in] cptr Client.
 * @param[in,out] st Its exchange.
 * @param[in] result What was decided.
 * @param[in] reason Text from the provider, or NULL.
 * @return Zero, or CPTR_KILLED.
 */
static int sasl_report_fail(struct Client* cptr, struct SaslState* st,
                            enum AccountResult result, const char* reason)
{
  if (st->ss_from_account) {
    send_reply(cptr, ERR_ACCOUNTFAIL,
               reason ? reason : _(cptr, account_strerror(result)));
    return sasl_fail(cptr, 0);
  }

  return sasl_fail(cptr, result == ACCOUNT_ERR_INUSE ? ERR_NICKLOCKED
                                                     : ERR_SASLFAIL);
}

/** Grant what a successful exchange earned, for a registered client.
 * @param[in] cptr Client.
 * @param[in] st Its exchange.
 * @return Zero, or CPTR_KILLED.
 */
static int sasl_succeed(struct Client* cptr, struct SaslState* st)
{
  if (!account_login(cptr, st->ss_account, st->ss_email)) {
    /* The nickname went to somebody else between the answer being asked
     * for and its arriving.  Nothing has changed, and the client is told
     * so in the one numeric that says exactly that.
     */
    return sasl_report_fail(cptr, st, ACCOUNT_ERR_INUSE, NULL);
  }

  sasl_tell_logged_in(cptr, st);

  return 0;
}

/** Apply a login that was held until registration finished.
 *
 * Called from register_user() once the client is a user.  Until then it
 * is not one, and a user mode cannot be granted to something that is not.
 * The client was told it was logged in when the answer arrived; what is
 * left here is the mode and the address, and neither is visible to it
 * before the welcome anyway.
 *
 * @param[in,out] cptr Client that has just registered.
 */
void sasl_registered(struct Client* cptr)
{
  struct SaslState* st;

  if (!MyConnect(cptr) || !cli_connect(cptr))
    return;

  if (!(st = cli_sasl(cptr)) || !st->ss_deferred)
    return;

  st->ss_deferred = 0;

  if (!account_login(cptr, st->ss_account, st->ss_email)) {
    /* The nickname was taken between the exchange and the welcome, which
     * account_claim_nick() should have made impossible.  Say so rather
     * than leave a client believing it is identified when it is not.
     */
    log_write(LS_USER, L_ERROR, 0, "Could not complete SASL login for %s "
              "as %s", get_client_name(cptr, HIDE_IP), st->ss_account);
    sasl_report_fail(cptr, st, ACCOUNT_ERR_INUSE, NULL);
  }
}

/** Take the answer to a credential check.
 * @param[in] cptr Client it was about, or NULL if it has left.
 * @param[in] result What the provider decided.
 * @param[in] nick Account nickname on success.
 * @param[in] email Address of the identity on success.
 * @param[in] reason Text for the user, or NULL.
 * @param[in] data Unused.
 */
static void sasl_answer(struct Client* cptr, enum AccountResult result,
                        const char* nick, const char* email,
                        const char* reason, void* data)
{
  struct SaslState* st;

  (void) data;

  sasl_answer_ran = 1;
  sasl_answer_killed = 0;

  if (!cptr || !MyConnect(cptr) || !cli_connect(cptr))
    return;

  if (!(st = cli_sasl(cptr)))
    return;

  st->ss_query = 0;

  if (result != ACCOUNT_OK || EmptyString(nick)) {
    log_write(LS_USER, L_INFO, 0, "SASL failed for %s: %s",
              get_client_name(cptr, HIDE_IP),
              reason ? reason : account_strerror(result));

    sasl_answer_killed = sasl_report_fail(cptr, st, result, reason);
    return;
  }

  ircd_strncpy(st->ss_account, nick, NICKLEN);
  ircd_strncpy(st->ss_email, email ? email : "", ACCOUNT_EMAIL_MAX);

  if (IsRegistered(cptr)) {
    sasl_answer_killed = sasl_succeed(cptr, st);
    return;
  }

  /* Before registration the client is not a user, so +r has to wait.  The
   * nickname does not: taking it now is what gets the client introduced
   * to the network under the name it is entitled to, instead of joining
   * as one thing and being renamed a moment later.
   */
  if (!account_claim_nick(cptr, nick)) {
    sasl_answer_killed = sasl_report_fail(cptr, st, ACCOUNT_ERR_INUSE, NULL);
    return;
  }

  /* Told now, granted later: see sasl_tell_logged_in(). */
  sasl_tell_logged_in(cptr, st);
  st->ss_deferred = 1;

  if (cli_auth(cptr))
    sasl_answer_killed = auth_sasl_done(cli_auth(cptr));
}

/** Hand a credential to the identity provider.
 *
 * @param[in] cptr Client.
 * @param[in,out] st Its exchange.
 * @param[in] req The credential.
 * @param[out] asked Set non-zero if the question was taken -- which
 *   includes its having been answered from inside this call.  Zero means
 *   nobody took it and the caller has to say so.
 * @return Zero, or CPTR_KILLED.
 */
static int sasl_submit(struct Client* cptr, struct SaslState* st,
                       const struct AccountRequest* req, int* asked)
{
  account_id_t id;

  sasl_answer_ran = 0;
  sasl_answer_killed = 0;

  id = account_verify(cptr, req, sasl_answer, 0);

  /* Answered from inside that call.  sasl_answer() has done everything
   * there is to do, including possibly exiting the client -- which frees
   * both it and \a st -- so neither may be touched from here.
   */
  if (sasl_answer_ran) {
    *asked = 1;
    return sasl_answer_killed;
  }

  if (!id) {
    *asked = 0;
    return 0;
  }

  st->ss_query = id;
  *asked = 1;

  /* The password has been handed on; this copy of it has no further use. */
  sasl_session_clear(&st->ss_session);

  return 0;
}

/** Hand a completed SASL credential to the identity provider.
 * @param[in] cptr Client.
 * @param[in,out] st Its exchange.
 * @return Zero, or CPTR_KILLED.
 */
static int sasl_ask(struct Client* cptr, struct SaslState* st)
{
  struct SaslSession* ses = &st->ss_session;
  struct AccountRequest req;
  int asked = 0;
  int res;

  memset(&req, 0, sizeof(req));
  req.ar_mech = ses->ss_mech ? ses->ss_mech->sm_name : "";
  req.ar_authcid = ses->ss_authcid;
  req.ar_authzid = ses->ss_authzid;
  req.ar_secret = ses->ss_secret;
  req.ar_secretlen = ses->ss_secretlen;
  req.ar_fingerprint = ses->ss_fingerprint;
  req.ar_ip = ircd_ntoa(&cli_ip(cptr));
  req.ar_tls = ses->ss_tls;

  if ((res = sasl_submit(cptr, st, &req, &asked)))
    return res;

  if (!asked)
    return sasl_fail(cptr, ERR_SASLFAIL);

  return 0;
}

/** Hand the identity provider a credential that came from ACCOUNT LOGIN.
 *
 * ACCOUNT asks the same question AUTHENTICATE does, and everything after
 * the question is the same too -- the answer, the @c +r grant, the
 * nickname, the hold on registration -- so it is the same code.  What
 * differs is which numerics the client is answered in, and that is one
 * flag on the state.
 *
 * @param[in,out] cptr Client that sent the command; must be local.
 * @param[in] authcid The address.
 * @param[in] authzid Which of its accounts, or "" for the default.
 * @param[in] secret The password.
 * @param[out] how What happened, from #SaslLogin.
 * @return Zero, or CPTR_KILLED.
 */
int sasl_login_request(struct Client* cptr, const char* authcid,
                       const char* authzid, const char* secret,
                       enum SaslLogin* how)
{
  struct SaslState* st;
  struct AccountRequest req;
  int asked = 0;
  int res;

  assert(0 != cptr);
  assert(0 != how);
  assert(MyConnect(cptr));

  *how = SASL_LOGIN_UNAVAILABLE;

  if (!account_have_provider())
    return 0;

  /* The password crossed the link as the client typed it. */
  if (!IsTLS(cptr) && feature_bool(FEAT_ACCOUNT_REQUIRE_TLS)) {
    *how = SASL_LOGIN_NOTLS;
    return 0;
  }

  st = sasl_state(cptr);

  /* An exchange under way, or an answer on its way: either one is a
   * second credential about one connection. */
  if (st->ss_query || st->ss_session.ss_mech) {
    *how = SASL_LOGIN_BUSY;
    return 0;
  }

  /* Before registration the grant is already deferred and the nickname
   * already claimed; a second login would have nothing left to apply.
   * After it, a login is how an account is switched, so it is allowed. */
  if (st->ss_done && !IsRegistered(cptr)) {
    *how = SASL_LOGIN_ALREADY;
    return 0;
  }

  if (st->ss_attempts >= SASL_MAX_ATTEMPTS) {
    *how = SASL_LOGIN_TOOMANY;
    return 0;
  }

  ++st->ss_attempts;
  st->ss_done = 0;
  st->ss_from_account = 1;

  memset(&req, 0, sizeof(req));
  req.ar_mech = "ACCOUNT";
  req.ar_authcid = authcid;
  req.ar_authzid = authzid ? authzid : "";
  req.ar_secret = secret;
  req.ar_secretlen = strlen(secret);
  req.ar_fingerprint = cli_tls_fingerprint(cptr);
  req.ar_ip = ircd_ntoa(&cli_ip(cptr));
  req.ar_tls = IsTLS(cptr) ? 1 : 0;

  /* The hold goes on before the question, not after: a provider that
   * answers from inside sasl_submit() releases it in that same call, and
   * a release that arrives before its hold leaves registration waiting
   * for an answer that has already come.
   */
  if (!IsRegistered(cptr) && cli_auth(cptr))
    auth_sasl_start(cli_auth(cptr));

  if ((res = sasl_submit(cptr, st, &req, &asked))) {
    *how = SASL_LOGIN_ASKED;
    return res;
  }

  if (!asked) {
    *how = SASL_LOGIN_UNAVAILABLE;
    st->ss_from_account = 0;
    return sasl_fail(cptr, 0);
  }

  *how = SASL_LOGIN_ASKED;

  return 0;
}

/** Handle AUTHENTICATE from a client, registered or not.
 *
 * @param[in] cptr Client that sent the line.
 * @param[in] sptr Source; the same client.
 * @param[in] parc Number of parameters.
 * @param[in] parv The parameters.
 * @return Zero, or CPTR_KILLED.
 */
int m_authenticate(struct Client* cptr, struct Client* sptr, int parc,
                   char* parv[])
{
  struct SaslState* st;
  enum SaslResult res;

  assert(0 != cptr);
  assert(cptr == sptr);

  if (parc < 2 || EmptyString(parv[1]))
    return send_reply(cptr, ERR_NEEDMOREPARAMS, "AUTHENTICATE");

  /* The capability is what says this conversation may happen at all: a
   * client that did not ask for it is a client the server has promised
   * byte-identical traffic to.
   */
  if (!CapActive(cptr, CAP_SASL))
    return send_reply(cptr, ERR_SASLFAIL);

  if (!account_have_provider())
    return send_reply(cptr, ERR_SASLFAIL);

  st = sasl_state(cptr);

  if (st->ss_done)
    return send_reply(cptr, ERR_SASLALREADY);

  /* An answer is on its way; another line now would be a second question
   * about the same connection. */
  if (st->ss_query)
    return send_reply(cptr, ERR_SASLFAIL);

  /* No mechanism chosen yet, so this line names one. */
  if (!st->ss_session.ss_mech) {
    char mechs[CAPVALUELEN];
    int err;

    if (!strcmp(parv[1], "*"))
      return sasl_fail(cptr, ERR_SASLABORTED);

    if (++st->ss_attempts > SASL_MAX_ATTEMPTS)
      return sasl_fail(cptr, ERR_SASLFAIL);

    st->ss_session.ss_tls = IsTLS(cptr) ? 1 : 0;
    ircd_strncpy(st->ss_session.ss_fingerprint, cli_tls_fingerprint(cptr),
                 sizeof(st->ss_session.ss_fingerprint) - 1);

    err = sasl_session_begin(&st->ss_session, parv[1]);

    if (err == -2 && !feature_bool(FEAT_ACCOUNT_REQUIRE_TLS)) {
      /* The operator has said in so many words that a password in the
       * clear is acceptable here.  Ask again with the requirement off.
       */
      st->ss_session.ss_tls = 1;
      err = sasl_session_begin(&st->ss_session, parv[1]);
    }

    if (err) {
      /* 908 first, so a client that picked a mechanism this server does
       * not have can pick another without guessing. */
      sasl_mechanisms_str(mechs, sizeof(mechs));
      send_reply(cptr, RPL_SASLMECHS, mechs);
      return sasl_fail(cptr, ERR_SASLFAIL);
    }

    /* Registration waits from here: the credential has not been sent
     * yet, but the client has committed to sending one, and letting it
     * in before the answer would log it in a moment after it arrived. */
    if (!IsRegistered(cptr) && cli_auth(cptr))
      auth_sasl_start(cli_auth(cptr));

    sendrawto_one(cptr, MSG_AUTHENTICATE " +");
    return 0;
  }

  res = sasl_session_input(&st->ss_session, parv[1]);

  switch (res) {
  case SASL_NEED_MORE:
    return 0;

  case SASL_CHALLENGE: {
    char out[SASL_WIRE_MAX + 1];

    if (ircd_base64_encode(st->ss_session.ss_out, st->ss_session.ss_outlen,
                           out, sizeof(out)) < 0)
      return sasl_fail(cptr, ERR_SASLFAIL);

    /* A challenge longer than one line would need the same chunking the
     * client uses; no mechanism in the core produces one, and sending
     * half of it would be worse than refusing. */
    if (strlen(out) > SASL_CHUNKLEN)
      return sasl_fail(cptr, ERR_SASLTOOLONG);

    sendrawto_one(cptr, MSG_AUTHENTICATE " %s", *out ? out : "+");
    return 0;
  }

  case SASL_CREDENTIAL:
    return sasl_ask(cptr, st);

  case SASL_ABORTED:
    return sasl_fail(cptr, ERR_SASLABORTED);

  case SASL_TOO_LONG:
    return sasl_fail(cptr, ERR_SASLTOOLONG);

  default:
    return sasl_fail(cptr, ERR_SASLFAIL);
  }
}
