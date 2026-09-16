/*
 * IRC - Internet Relay Chat, ircd/account.c
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
 * @brief The identity provider register and the questions in flight.
 *
 * Half of the account subsystem: this one asks and holds, and never
 * dereferences a client.  What an answer does to a user is in
 * ircd/account_user.c.  See include/account.h for why.
 */
#include "config.h"

#include "account.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_events.h"
#include "ircd_features.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_string.h"
#include "random.h"
#include "sasl.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** One question the provider has not answered yet. */
struct AccountCall {
  struct AccountCall* ac_next;      /**< Next question, in no order. */
  account_id_t        ac_id;        /**< What the provider holds. */
  struct Client*      ac_client;    /**< Who it is about; never read. */
  time_t              ac_deadline;  /**< When it is given up on. */
  AccountDoneFn       ac_done;      /**< For a credential check. */
  AccountOwnerFn      ac_owner;     /**< For a nickname lookup. */
  AccountListFn       ac_list;      /**< For a listing. */
  void*               ac_data;      /**< The caller's opaque pointer. */
  char                ac_nick[NICKLEN + 1]; /**< Nickname a lookup asked about. */
  char                ac_email[ACCOUNT_EMAIL_MAX + 1]; /**< Address a listing asked about. */
};

/** The registered provider, or NULL. */
static const struct AccountProvider* account_provider;
/** Module that registered it. */
static struct ModuleHandle* account_provider_owner;

/** Questions accepted and not yet answered. */
static struct AccountCall* account_calls;
/** How many are on that list. */
static unsigned int account_num;
/** Handle for the next question; never zero, never reused while in flight. */
static account_id_t account_last_id;

/** Ceiling on questions in flight.  One per client authenticating is the
 * normal case, and the connection count already bounds that; the cap is
 * for a provider that accepts and never answers.
 */
#define ACCOUNT_PENDING_MAX 16384

/** Timer over the earliest deadline, armed only while something is asked.
 *
 * Its own timer for the reason hooks.c has one: check_pings() is scheduled
 * minutes ahead on an idle server, and a deadline that is only enforced
 * eventually is not a deadline.  Nothing is armed while nothing is in
 * flight, so a server with no identity module pays for none of this.
 */
static struct Timer account_timer;

/** Whether #account_timer is on the queue. */
static int account_timer_armed;

static void account_timeout(struct Event* ev);

/** Earliest deadline of anything in flight, or 0 if there is nothing. */
static time_t account_deadline(void)
{
  struct AccountCall* call;
  time_t earliest = 0;

  for (call = account_calls; call; call = call->ac_next)
    if (!earliest || call->ac_deadline < earliest)
      earliest = call->ac_deadline;

  return earliest;
}

/** Make sure the timer will fire by the earliest deadline.
 *
 * Every question gets the same FEAT_ACCOUNT_TIMEOUT, so a new one is
 * always later than what the timer is set for; the other direction leaves
 * the timer early, which costs one callback that finds nothing to do.
 */
static void account_arm(void)
{
  time_t deadline;

  if (account_timer_armed)
    return;

  if (!(deadline = account_deadline()))
    return;

  timer_add(timer_init(&account_timer), account_timeout, 0, TT_ABSOLUTE,
            deadline);
  account_timer_armed = 1;
}

/** Fail whatever has waited too long, and set the timer for the rest. */
static void account_timeout(struct Event* ev)
{
  if (ev_type(ev) != ET_EXPIRE) {
    if (ev_type(ev) == ET_DESTROY)
      account_timer_armed = 0;
    return;
  }

  /* An absolute timer is done once it has expired; re-arming is a fresh
   * timer_add(), the same way check_pings() does it.
   */
  account_timer_armed = 0;

  account_expire(CurrentTime);
  account_arm();
}

/** Characters a guest name's random part is drawn from. */
static const char account_b62[] =
  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/** How many of them one guest name gets.
 *
 * Eight is 2.2e14 names.  The point is not to make a collision unlikely
 * but to make it not happen: what the server does about one is kill the
 * client, and a retry loop around something that never runs is code that
 * gets written, never tested, and one day executed.
 */
#define ACCOUNT_GUEST_RANDOM 8

/** Text for each #AccountResult, indexed by the enum.
 *
 * Marked for translation but never translated here: this half of the
 * subsystem has no client to translate for, and the reason reaches a user
 * as a parameter of a numeric.  Whoever shows it to somebody wraps it in
 * _(), and the catalog has the entry because N_() put it in the template.
 */
static const char* account_result_text[ACCOUNT_ERR_LAST] = {
  N_("authentication succeeded"),
  N_("no such account, or the password is wrong"),
  N_("that identity has no such account"),
  N_("that account is suspended"),
  N_("somebody else is using that nickname"),
  N_("the identity service is not available"),
  N_("the identity service did not answer in time")
};

/** Text for a result, for a log line or a default reason.
 * @param[in] result What was decided.
 */
const char* account_strerror(enum AccountResult result)
{
  if (result < 0 || result >= ACCOUNT_ERR_LAST)
    return "unknown error";

  return account_result_text[result];
}

/** Find a question by handle. */
static struct AccountCall* account_find(account_id_t id)
{
  struct AccountCall* call;

  for (call = account_calls; call; call = call->ac_next)
    if (call->ac_id == id)
      return call;

  return NULL;
}

/** Hand out a handle that is neither zero nor already in flight. */
static account_id_t account_new_id(void)
{
  do {
    ++account_last_id;
  } while (account_last_id == 0 || account_find(account_last_id));

  return account_last_id;
}

/** Unlink and free one question.  Does not call anything back. */
static void account_free(struct AccountCall* call)
{
  struct AccountCall** call_p;

  for (call_p = &account_calls; *call_p; call_p = &(*call_p)->ac_next) {
    if (*call_p == call) {
      *call_p = call->ac_next;
      account_num--;
      break;
    }
  }

  MyFree(call);
}

/** Take a question off the list and answer it.
 *
 * Off the list before the callback runs: the callback usually ends up
 * renaming or killing the client, which comes back through
 * account_cancel_client(), and it must not find this entry still linked.
 */
static void account_answer(struct AccountCall* call, enum AccountResult result,
                           enum AccountOwner owner, const char* nick,
                           const char* email, const char* reason)
{
  AccountDoneFn done = call->ac_done;
  AccountOwnerFn ownerfn = call->ac_owner;
  AccountListFn listfn = call->ac_list;
  struct Client* cptr = call->ac_client;
  void* data = call->ac_data;
  char nickbuf[NICKLEN + 1];

  ircd_strncpy(nickbuf, call->ac_nick, NICKLEN);

  account_free(call);

  if (done)
    (*done)(cptr, result, nick, email, reason, data);
  else if (ownerfn)
    (*ownerfn)(cptr, owner, nickbuf, data);
  else if (listfn)
    (*listfn)(cptr, result, NULL, 0, reason, data);
}

/** Take a listing off the list and answer it.
 *
 * Its own function rather than a case of account_answer(), because the
 * entries are a parameter that no other kind of answer has and that
 * nothing here may keep.
 */
static void account_answer_list(struct AccountCall* call,
                                enum AccountResult result,
                                const struct AccountEntry* entries,
                                unsigned int count, const char* reason)
{
  AccountListFn listfn = call->ac_list;
  struct Client* cptr = call->ac_client;
  void* data = call->ac_data;

  account_free(call);

  if (listfn)
    (*listfn)(cptr, result, entries, count, reason, data);
}

/** Register the loaded module as the identity provider.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] provider Static description of the provider.
 * @return Non-zero on success.
 */
int account_register_provider(struct ModuleHandle* mod,
                              const struct AccountProvider* provider)
{
  /* All three questions, not just the one a provider happens to care
   * about.  A provider that can say whether a credential is good can say
   * what accounts an address holds, and one that could not would make
   * ACCOUNT LIST answer "the identity service is not available" on a
   * server where identity works -- which is the one answer a user cannot
   * tell from an outage.
   */
  if (!provider || !provider->ap_name || !provider->ap_verify
      || !provider->ap_lookup || !provider->ap_list) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Refusing an identity provider that is missing a name, "
              "ap_verify, ap_lookup or ap_list");
    return 0;
  }

  if (account_provider) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Refusing identity provider %s: %s is already registered",
              provider->ap_name, account_provider->ap_name);
    return 0;
  }

  account_provider = provider;
  account_provider_owner = mod;

  log_write(LS_SYSTEM, L_INFO, 0, "Identity provider %s registered",
            provider->ap_name);

  /* SASL is only worth offering once somebody can answer for it. */
  sasl_advertise();

  return 1;
}

/** Withdraw the provider.
 * @param[in] mod Handle that registered it.
 */
void account_unregister_provider(struct ModuleHandle* mod)
{
  const char* name;

  if (!account_provider || account_provider_owner != mod)
    return;

  name = account_provider->ap_name;

  /* The register goes first, so that a callback which tries to ask
   * something on its way out is refused rather than reaching a module
   * that is being unmapped.
   */
  account_provider = NULL;
  account_provider_owner = NULL;

  while (account_calls) {
    struct AccountCall* call = account_calls;

    log_write(LS_SYSTEM, L_INFO, 0,
              "Identity provider %s went away with a question outstanding",
              name);

    account_answer(call, ACCOUNT_ERR_UNAVAILABLE, ACCOUNT_NICK_UNKNOWN,
                   NULL, NULL, NULL);
  }

  log_write(LS_SYSTEM, L_INFO, 0, "Identity provider %s withdrawn", name);

  sasl_advertise();
}

/** Non-zero if a provider is registered and can be asked. */
int account_have_provider(void)
{
  return account_provider != NULL;
}

/** Name of the registered provider, or "none". */
const char* account_provider_name(void)
{
  return account_provider ? account_provider->ap_name : "none";
}

/** Start one question, with everything but the provider call done.
 * @return The new entry, or NULL if there is no provider or no room.
 */
static struct AccountCall* account_begin(struct Client* cptr,
                                         AccountDoneFn done,
                                         AccountOwnerFn owner,
                                         AccountListFn list, void* data)
{
  struct AccountCall* call;

  if (!account_provider)
    return NULL;

  if (account_num >= ACCOUNT_PENDING_MAX) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Identity provider %s has %u questions outstanding; refusing "
              "another", account_provider->ap_name, account_num);
    return NULL;
  }

  call = (struct AccountCall*) MyCalloc(1, sizeof(struct AccountCall));
  call->ac_id = account_new_id();
  call->ac_client = cptr;
  call->ac_deadline = CurrentTime + feature_int(FEAT_ACCOUNT_TIMEOUT);
  call->ac_done = done;
  call->ac_owner = owner;
  call->ac_list = list;
  call->ac_data = data;

  call->ac_next = account_calls;
  account_calls = call;
  account_num++;

  account_arm();

  return call;
}

/** Ask whether a credential is good.
 * @param[in] cptr Client it is about.
 * @param[in] req The credential.
 * @param[in] done Called with the answer.
 * @param[in] data Opaque pointer for \a done.
 * @return The handle, or 0 if there is no provider.
 */
account_id_t account_verify(struct Client* cptr,
                            const struct AccountRequest* req,
                            AccountDoneFn done, void* data)
{
  struct AccountCall* call;
  account_id_t id;

  assert(0 != req);
  assert(0 != done);

  if (!(call = account_begin(cptr, done, NULL, NULL, data)))
    return 0;

  id = call->ac_id;

  /* The provider may answer from inside this call -- nothing stops it,
   * and a provider with a cache will -- so the handle is read out before
   * the entry can be freed underneath us.
   */
  (*account_provider->ap_verify)(id, req);

  return id;
}

/** Ask who a nickname belongs to.
 * @param[in] cptr Client it is about, or NULL.
 * @param[in] nick Nickname to ask about.
 * @param[in] done Called with the answer.
 * @param[in] data Opaque pointer for \a done.
 * @return The handle, or 0 if there is no provider.
 */
account_id_t account_lookup(struct Client* cptr, const char* nick,
                            AccountOwnerFn done, void* data)
{
  struct AccountCall* call;
  account_id_t id;

  assert(0 != done);

  if (EmptyString(nick))
    return 0;

  if (!(call = account_begin(cptr, NULL, done, NULL, data)))
    return 0;

  ircd_strncpy(call->ac_nick, nick, NICKLEN);
  id = call->ac_id;

  (*account_provider->ap_lookup)(id, call->ac_nick);

  return id;
}

/** Ask what accounts an address holds.
 * @param[in] cptr Client it is about.
 * @param[in] email The address, as authenticated.
 * @param[in] done Called with the answer.
 * @param[in] data Opaque pointer for \a done.
 * @return The handle, or 0 if there is no provider.
 */
account_id_t account_list(struct Client* cptr, const char* email,
                          AccountListFn done, void* data)
{
  struct AccountCall* call;
  account_id_t id;

  assert(0 != done);

  if (EmptyString(email))
    return 0;

  if (!(call = account_begin(cptr, NULL, NULL, done, data)))
    return 0;

  ircd_strncpy(call->ac_email, email, ACCOUNT_EMAIL_MAX);
  id = call->ac_id;

  (*account_provider->ap_list)(id, call->ac_email);

  return id;
}

/** Answer a credential check.
 * @param[in] id Handle the provider was given.
 * @param[in] result What was decided.
 * @param[in] nick Account nickname on success.
 * @param[in] email Address of the identity on success.
 * @param[in] reason Text for the user, or NULL.
 * @return Non-zero if the handle was outstanding.
 */
int account_complete(account_id_t id, enum AccountResult result,
                     const char* nick, const char* email, const char* reason)
{
  struct AccountCall* call = account_find(id);

  if (!call || !call->ac_done) {
    log_write(LS_SYSTEM, L_INFO, 0,
              "account_complete() for %lu, which is no longer outstanding",
              (unsigned long) id);
    return 0;
  }

  account_answer(call, result, ACCOUNT_NICK_UNKNOWN, nick, email, reason);

  return 1;
}

/** Answer a nickname lookup.
 * @param[in] id Handle the provider was given.
 * @param[in] owner What was established.
 * @return Non-zero if the handle was outstanding.
 */
int account_complete_owner(account_id_t id, enum AccountOwner owner)
{
  struct AccountCall* call = account_find(id);

  if (!call || !call->ac_owner) {
    log_write(LS_SYSTEM, L_INFO, 0,
              "account_complete_owner() for %lu, which is no longer "
              "outstanding", (unsigned long) id);
    return 0;
  }

  account_answer(call, ACCOUNT_ERR_UNAVAILABLE, owner, NULL, NULL, NULL);

  return 1;
}

/** Answer a listing.
 * @param[in] id Handle the provider was given.
 * @param[in] result #ACCOUNT_OK, or why there is no listing.
 * @param[in] entries The accounts; not kept past this call.
 * @param[in] count How many.
 * @param[in] reason Text for the user, or NULL.
 * @return Non-zero if the handle was outstanding.
 */
int account_complete_list(account_id_t id, enum AccountResult result,
                          const struct AccountEntry* entries,
                          unsigned int count, const char* reason)
{
  struct AccountCall* call = account_find(id);

  if (!call || !call->ac_list) {
    log_write(LS_SYSTEM, L_INFO, 0,
              "account_complete_list() for %lu, which is no longer "
              "outstanding", (unsigned long) id);
    return 0;
  }

  account_answer_list(call, result, entries, count, reason);

  return 1;
}

/** Drop every question about a client, without answering any. */
void account_cancel_client(struct Client* cptr)
{
  struct AccountCall* call;
  struct AccountCall* next;

  if (!cptr)
    return;

  for (call = account_calls; call; call = next) {
    next = call->ac_next;

    if (call->ac_client != cptr)
      continue;

    /* Tell the provider to stop: it may have a query running, and the
     * answer would otherwise come back to a handle nobody holds.
     */
    if (account_provider && account_provider->ap_cancel)
      (*account_provider->ap_cancel)(call->ac_id);

    account_free(call);
  }
}

/** Fail every question whose deadline has passed.
 *
 * One at a time, restarting the walk after each: the callback can rename
 * or kill a client, and that removes further entries from this list.
 *
 * @param[in] now Current time.
 * @return How many expired.
 */
int account_expire(time_t now)
{
  struct AccountCall* call;
  int expired = 0;

  for (;;) {
    for (call = account_calls; call; call = call->ac_next)
      if (call->ac_deadline <= now)
        break;

    if (!call)
      break;

    log_write(LS_SYSTEM, L_ERROR, 0,
              "Identity provider %s did not answer question %lu in time",
              account_provider_name(), (unsigned long) call->ac_id);

    if (account_provider && account_provider->ap_cancel)
      (*account_provider->ap_cancel)(call->ac_id);

    account_answer(call, ACCOUNT_ERR_TIMEOUT, ACCOUNT_NICK_UNKNOWN,
                   NULL, NULL, NULL);
    expired++;
  }

  return expired;
}

/** Questions currently in flight. */
unsigned int account_pending_count(void)
{
  return account_num;
}

/** Release the register; main() only, at exit. */
void account_close(void)
{
  while (account_calls) {
    struct AccountCall* call = account_calls;

    account_calls = call->ac_next;
    MyFree(call);
  }

  account_num = 0;
  account_provider = NULL;
  account_provider_owner = NULL;

  if (account_timer_armed) {
    timer_del(&account_timer);
    account_timer_armed = 0;
  }
}

/** Write a guest nickname into \a buf.
 * @param[out] buf Buffer for the nickname.
 * @param[in] len Its size, terminator included.
 * @return Non-zero on success.
 */
int account_guest_nick(char* buf, size_t len)
{
  const char* prefix = feature_str(FEAT_GUEST_PREFIX);
  size_t plen;
  size_t i;

  assert(0 != buf);

  if (EmptyString(prefix))
    prefix = "guest-";

  plen = strlen(prefix);

  /* The name has to fit in a nickname twice over: in the buffer, and in
   * what the network will carry.  A prefix long enough to squeeze out the
   * random part would turn every guest into the same guest.
   */
  if (plen + ACCOUNT_GUEST_RANDOM >= len
      || plen + ACCOUNT_GUEST_RANDOM > NICKLEN)
    return 0;

  memcpy(buf, prefix, plen);

  for (i = 0; i < ACCOUNT_GUEST_RANDOM; i++)
    buf[plen + i] = account_b62[ircrandom() % (sizeof(account_b62) - 1)];

  buf[plen + ACCOUNT_GUEST_RANDOM] = '\0';

  return 1;
}
