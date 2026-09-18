/*
 * IRC - Internet Relay Chat, modules/services/irc_services/nick_policy.c
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
 * @brief The grace period: what happens to somebody using a registered nick.
 *
 * This is the policy half of proposal 007 -- the half that decides, warns
 * and renames.  The mechanism is elsewhere and stays there: the core owns
 * @c +f, @c +r and the @c guest-* rename (account.h), and the identity
 * module owns the store.  What is here is the rule, which is:
 *
 *   a local client that is using a registered nickname and has not proved
 *   the nickname is its own is frozen, warned, and renamed to @c guest-*
 *   when the grace period runs out.
 *
 * It is a grace period and not a veto because a veto would have to wait
 * for a database, and the two points where the question arises --
 * registration and a nick change -- are not points the server can be held
 * at.  So the client gets the nick and loses it again, rather than being
 * held at the door while somebody asks PostgreSQL.
 *
 * Three things end a freeze, and this file is only one of them:
 *
 *   - the client identifies: @c account_login() grants @c +r, and
 *     @c do_user_mode() clears @c +f in the same breath, because +r is
 *     exactly the proof +f says is missing.  Nothing here runs.
 *   - the grace period expires: the timer below renames to @c guest-*.
 *   - the client changes to a nickname nobody has registered: the lookup
 *     below comes back free and the freeze is lifted.
 *
 * And the rule that overrides the rest (proposal 007 section 7): when the
 * owner of a nickname @b cannot @b be @b established -- the database is
 * down, the provider is gone, the deadline passed -- the nickname is not
 * usable.  #ACCOUNT_NICK_UNKNOWN is never read as "free".  At registration
 * that means the client comes in as @c guest-*; at a nick change it means
 * the client is put back under the name it had, which grants it nothing it
 * did not already have.
 *
 * None of this happens at all on a server with no identity provider.  The
 * rule above is about a service that has failed, not about a network that
 * never had accounts: freezing everybody on a server that was never going
 * to answer would be reading "no accounts exist" as "every nick is
 * somebody else's".
 */
#include "config.h"

#include "services.h"

#include "account.h"
#include "bot.h"
#include "client.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_events.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "hash.h"
#include "module.h"
#include "numeric.h"
#include "s_conf.h"
#include "s_misc.h"
#include "s_user.h"
#include "sasl.h"
#include "send.h"
#include "struct.h"

#include <string.h>

/** Seconds a client gets to identify, when the block does not say. */
#define NICKPOLICY_GRACE_DEFAULT 60

/** Longest grace period the block may ask for.
 *
 * Ten minutes.  A frozen client can do nothing but identify, so a grace
 * period long enough to be forgotten about is a client sitting on somebody
 * else's nickname for as long as it likes.
 */
#define NICKPOLICY_GRACE_MAX 600

/** One client waiting to prove a nickname is its own. */
struct NickHold {
  struct NickHold* nh_next;
  struct Client*   nh_client;    /**< Local, and still here. */
  time_t           nh_deadline;  /**< When it is renamed. */
};

/** Everything waiting. */
static struct NickHold* nick_holds;

/** Timer over the earliest deadline, armed only while something waits. */
static struct Timer nick_timer;

/** Whether #nick_timer is on the queue. */
static int nick_timer_armed;

/** Non-zero while this file is renaming somebody.
 *
 * Its own renames come back through HOOK_CLIENT_NICK_CHANGED like anybody
 * else's, and asking the database who owns the @c guest-* name we just
 * made up would be a query for an answer we already know.
 */
static int nick_renaming;

/** The grace period in force, from the Service{} block. */
static int nick_grace = NICKPOLICY_GRACE_DEFAULT;

static void nick_timeout(struct Event* ev);

/* ------------------------------------------------------------------- *
 * Holds                                                               *
 * ------------------------------------------------------------------- */

/** The hold on \a cptr, or NULL. */
static struct NickHold* nick_find(const struct Client* cptr)
{
  struct NickHold* hold;

  for (hold = nick_holds; hold; hold = hold->nh_next)
    if (hold->nh_client == cptr)
      return hold;

  return NULL;
}

/** Forget a hold without acting on it. */
static void nick_drop(struct Client* cptr)
{
  struct NickHold** p;

  for (p = &nick_holds; *p; p = &(*p)->nh_next) {
    if ((*p)->nh_client == cptr) {
      struct NickHold* hold = *p;

      *p = hold->nh_next;
      MyFree(hold);
      return;
    }
  }
}

/** Earliest deadline waiting, or 0. */
static time_t nick_earliest(void)
{
  struct NickHold* hold;
  time_t earliest = 0;

  for (hold = nick_holds; hold; hold = hold->nh_next)
    if (!earliest || hold->nh_deadline < earliest)
      earliest = hold->nh_deadline;

  return earliest;
}

/** Make sure the timer will fire by the earliest deadline.
 *
 * Its own timer, for the reason account.c and hooks.c each have one: a
 * deadline enforced whenever the server next happens to look round is not
 * a deadline.
 */
static void nick_arm(void)
{
  time_t deadline = nick_earliest();

  if (!deadline) {
    if (nick_timer_armed) {
      timer_del(&nick_timer);
      nick_timer_armed = 0;
    }
    return;
  }

  if (nick_timer_armed)
    timer_chg(&nick_timer, TT_ABSOLUTE, deadline);
  else {
    /* Never timer_init() here.  That zeroes the generator's flags,
     * GEN_MARKED among them, and GEN_MARKED is what tells timer_add() it
     * is being called from inside the timer's own expiry -- which is
     * where it is called from every time a rename starts something new.
     * Without it the timer is queued a second time while timer_run()
     * still holds it, and the server dies later on an event for a
     * generator that is no longer active.  Initialised once, in
     * nickpolicy_init(). */
    timer_add(&nick_timer, nick_timeout, NULL, TT_ABSOLUTE, deadline);
    nick_timer_armed = 1;
  }
}

/* ------------------------------------------------------------------- *
 * Talking to the user                                                 *
 * ------------------------------------------------------------------- */

/** Say something to \a cptr as NickServ, if there is a NickServ.
 *
 * Not static, because an answer that arrives long after the command that
 * asked for it -- which is every answer a write gets -- has no
 * #ServiceCall left to go through svc_reply().
 *
 * Silence when there is no bot is deliberate rather than a fallback to a
 * server notice: the freeze still happens, and a message from the server
 * about identifying to a service that is not running would send the user
 * somewhere there is nobody to answer.
 */
void nick_tell(struct Client* cptr, const char* fmt, ...)
{
  struct Client* bot = svc_bot_of_type("nickserv");
  char buf[BUFSIZE];
  va_list vl;

  if (!bot)
    return;

  va_start(vl, fmt);
  ircd_vsnprintf(cptr, buf, sizeof(buf), _(cptr, fmt), vl);
  va_end(vl);

  bot_send_user(bot, cptr, 1, buf);
}

/** Set or clear \a modes on \a cptr as NickServ.
 * @return Non-zero if the bot was there and entitled.
 */
static int nick_mode(struct Client* cptr, const char* modes)
{
  struct Client* bot = svc_bot_of_type("nickserv");

  if (!bot)
    return 0;

  return bot_set_user_mode(bot, cptr, modes);
}

/* ------------------------------------------------------------------- *
 * The three outcomes                                                  *
 * ------------------------------------------------------------------- */

/** Rename \a cptr to a guest name, telling it why first.
 * @param[in] cptr Client to rename.
 * @param[in] reason What to put in the KILL if the guest name is taken.
 */
static void nick_to_guest(struct Client* cptr, const char* reason)
{
  nick_drop(cptr);

  /* The mode first: account_force_guest() may end with the client gone,
   * and a -f nobody sees is better than a +f left on a client that is
   * about to be killed. */
  if (IsFrozen(cptr))
    nick_mode(cptr, "-f");

  nick_renaming++;
  account_force_guest(cptr, reason);
  nick_renaming--;

  nick_arm();
}

/** Freeze \a cptr and start its grace period.
 * @param[in] cptr Client using a registered nickname.
 */
static void nick_freeze(struct Client* cptr)
{
  struct NickHold* hold = nick_find(cptr);

  if (!hold) {
    hold = (struct NickHold*) MyCalloc(1, sizeof(*hold));
    hold->nh_client = cptr;
    hold->nh_next = nick_holds;
    nick_holds = hold;
  }

  hold->nh_deadline = CurrentTime + nick_grace;

  if (!nick_mode(cptr, "+f")) {
    /* No bot, so nobody can lift it either.  Freezing a client that
     * nothing can unfreeze would be a slower way of killing it. */
    nick_drop(cptr);
    nick_arm();
    return;
  }

  nick_tell(cptr, "This nickname is registered.  Identify with "
                  "/msg %s IDENTIFY <address> <password> within %d seconds, "
                  "or you will be renamed.",
            cli_name(svc_bot_of_type("nickserv")), nick_grace);

  nick_arm();
}

/** Lift a freeze this file put on.
 * @param[in] cptr Client that is free to keep its nickname.
 */
static void nick_release(struct Client* cptr)
{
  nick_drop(cptr);
  nick_arm();

  /* Frozen is the condition, not the hold: the hold is dropped as soon as
   * a nick change is seen, well before the answer about the new name
   * arrives, so it says nothing about what the client is wearing now. */
  if (!IsFrozen(cptr))
    return;

  nick_mode(cptr, "-f");

  nick_tell(cptr, "That nickname is not registered; you are free to use it.");
}

/* ------------------------------------------------------------------- *
 * Asking                                                              *
 * ------------------------------------------------------------------- */

/** What a lookup was asked for. */
enum NickWhen {
  NICK_AT_REGISTER,   /**< The client had just arrived. */
  NICK_AT_CHANGE      /**< The client had just changed nick. */
};

/** What a lookup is about, kept because the answer comes back later. */
struct NickAsk {
  struct NickAsk* na_next;
  struct Client*  na_client;
  enum NickWhen   na_when;
  char            na_previous[NICKLEN + 1]; /**< Nick before the change. */
};

/** Lookups in flight, so a client that leaves takes its own with it. */
static struct NickAsk* nick_asks;

/** Forget one lookup. */
static void nick_ask_free(struct NickAsk* ask)
{
  struct NickAsk** p;

  for (p = &nick_asks; *p; p = &(*p)->na_next) {
    if (*p == ask) {
      *p = ask->na_next;
      MyFree(ask);
      return;
    }
  }
}

/** Put \a cptr back under the name it had, or make it a guest.
 *
 * Proposal 007 section 7 at a nick change: the change is undone rather
 * than allowed, because keeping the name it already had grants the client
 * nothing new.  The old name can have gone in the meantime -- somebody
 * else taking it is exactly what a nick change frees it for -- and then
 * there is no name left that is known to be safe, so it is a guest.
 *
 * @param[in] cptr Client to put back.
 * @param[in] previous The nickname it had.
 */
static void nick_put_back(struct Client* cptr, const char* previous)
{
  char parv_nick[NICKLEN + 1];
  char* parv[3];

  if (EmptyString(previous) || FindClient(previous)) {
    nick_to_guest(cptr, "Nickname services are not available");
    return;
  }

  ircd_strncpy(parv_nick, previous, NICKLEN);

  parv[0] = cli_name(cptr);
  parv[1] = parv_nick;
  parv[2] = NULL;

  nick_renaming++;
  set_nick_name(cptr, cptr, parv_nick, 2, parv);
  nick_renaming--;
}

/** Take the answer to "whose nickname is this?".
 * @param[in] cptr Client it was about, or NULL if it has left.
 * @param[in] owner What was established.
 * @param[in] nick The nickname asked about.
 * @param[in] data The #NickAsk.
 */
static void nick_answer(struct Client* cptr, enum AccountOwner owner,
                        const char* nick, void* data)
{
  struct NickAsk* ask = (struct NickAsk*) data;
  enum NickWhen when = ask->na_when;
  char previous[NICKLEN + 1];

  ircd_strncpy(previous, ask->na_previous, NICKLEN);
  nick_ask_free(ask);

  if (!cptr || !MyUser(cptr) || IsBot(cptr) || IsServiceBot(cptr))
    return;

  /* The client identified while the question was in flight -- which is
   * what happens when the rename account_login() does is what asked it.
   * +r is the answer to the question, so there is nothing to decide. */
  if (IsAccount(cptr))
    return;

  /* It changed nick again while we were asking; a later answer is about a
   * name it is no longer using. */
  if (0 != ircd_strcmp(cli_name(cptr), nick))
    return;

  switch (owner) {
  case ACCOUNT_NICK_FREE:
    nick_release(cptr);
    break;

  case ACCOUNT_NICK_REGISTERED:
    nick_freeze(cptr);
    break;

  default:
    /* Proposal 007 section 7: not established is not free. */
    log_write(LS_USER, L_INFO, 0,
              "nickserv: could not establish who holds %s; %s",
              nick, when == NICK_AT_REGISTER ? "renaming to a guest"
                                             : "undoing the nick change");

    nick_tell(cptr, "Nickname services are not available, so this nickname "
                    "could not be checked.");

    if (when == NICK_AT_REGISTER)
      nick_to_guest(cptr, "Nickname services are not available");
    else
      nick_put_back(cptr, previous);
    break;
  }
}

/** Ask who holds the nickname \a cptr is using.
 * @param[in] cptr Client to ask about.
 * @param[in] when Why we are asking.
 * @param[in] previous Nickname it had before, or "".
 */
static void nick_ask(struct Client* cptr, enum NickWhen when,
                     const char* previous)
{
  struct NickAsk* ask;

  ask = (struct NickAsk*) MyCalloc(1, sizeof(*ask));
  ask->na_client = cptr;
  ask->na_when = when;
  ircd_strncpy(ask->na_previous, previous ? previous : "", NICKLEN);

  ask->na_next = nick_asks;
  nick_asks = ask;

  if (!account_lookup(cptr, cli_name(cptr), nick_answer, ask)) {
    /* No provider at all: refused here rather than answered, so this is
     * the one caller that has to turn it into an answer itself. */
    nick_answer(cptr, ACCOUNT_NICK_UNKNOWN, cli_name(cptr), ask);
  }
}

/* ------------------------------------------------------------------- *
 * The timer                                                           *
 * ------------------------------------------------------------------- */

/** Rename whoever ran out of time.
 *
 * One at a time, restarting the walk after each: renaming a client comes
 * back through the nick-change hook and through the exit path, and either
 * can remove further entries from this list.
 */
static void nick_timeout(struct Event* ev)
{
  struct NickHold* hold;

  if (ev_type(ev) != ET_EXPIRE) {
    if (ev_type(ev) == ET_DESTROY)
      nick_timer_armed = 0;
    return;
  }

  nick_timer_armed = 0;

  for (;;) {
    struct Client* cptr;

    for (hold = nick_holds; hold; hold = hold->nh_next)
      if (hold->nh_deadline <= CurrentTime)
        break;

    if (!hold)
      break;

    cptr = hold->nh_client;

    /* The hold is advisory, and the deadline is where it is checked
     * against what is actually true.  A client that identified is no
     * longer frozen -- do_user_mode() clears +f with the +r grant -- and
     * that happens without a nick change whenever the account's nickname
     * is the one already in use, which is the ordinary case.  There is
     * nothing here to be told about it, and nothing that needs to be.
     */
    if (!IsFrozen(cptr) || IsAccount(cptr)) {
      nick_drop(cptr);
      continue;
    }

    nick_tell(cptr, "You did not identify in time; you are being renamed.");
    nick_to_guest(cptr, "Did not identify to a registered nickname in time");
  }

  nick_arm();
}

/* ------------------------------------------------------------------- *
 * Hooks                                                               *
 * ------------------------------------------------------------------- */

/** Non-zero when the policy applies to \a cptr at all. */
static int nick_applies(struct Client* cptr)
{
  if (!cptr || !MyUser(cptr))
    return 0;

  /* A bot of this server is introduced by the server, under a name the
   * configuration chose; it has nothing to prove to anyone. */
  if (IsBot(cptr) || IsServiceBot(cptr))
    return 0;

  /* Already identified, so the nickname is provably its own. */
  if (IsAccount(cptr))
    return 0;

  /* Nobody to ask.  See the note at the top of this file: no provider is
   * not a failed lookup. */
  if (!account_have_provider())
    return 0;

  return 1;
}

/** A local client finished registering. */
static enum HookResult nick_on_registered(struct HookContext* ctx, void* user)
{
  (void) user;

  if (nick_applies(ctx->hc_client))
    nick_ask(ctx->hc_client, NICK_AT_REGISTER, "");

  return HOOK_CONTINUE;
}

/** A client changed nick; hc_arg is the name it had. */
static enum HookResult nick_on_nick(struct HookContext* ctx, void* user)
{
  struct Client* cptr = ctx->hc_client;
  const char* previous = ctx->hc_arg;

  (void) user;

  /* Our own rename, whose answer we already know. */
  if (nick_renaming)
    return HOOK_CONTINUE;

  if (!nick_applies(cptr))
    return HOOK_CONTINUE;

  /* A change of capitals is the same nickname to every server on the
   * network, so nothing about who owns it has changed either. */
  if (previous && 0 == ircd_strcmp(previous, cli_name(cptr)))
    return HOOK_CONTINUE;

  /* Whatever was decided about the old name does not apply to this one. */
  nick_drop(cptr);
  nick_arm();

  nick_ask(cptr, NICK_AT_CHANGE, previous);

  return HOOK_CONTINUE;
}

/** A client is leaving: it takes its hold and its question with it. */
static enum HookResult nick_on_exiting(struct HookContext* ctx, void* user)
{
  struct NickAsk* ask;
  struct NickAsk* next;

  (void) user;

  nick_drop(ctx->hc_client);
  nick_arm();

  for (ask = nick_asks; ask; ask = next) {
    next = ask->na_next;

    if (ask->na_client == ctx->hc_client)
      nick_ask_free(ask);
  }

  return HOOK_CONTINUE;
}

/* ------------------------------------------------------------------- *
 * Configuration and lifecycle                                         *
 * ------------------------------------------------------------------- */

/** Re-read the Service{} block's options.  Start-up and every rehash. */
void nickpolicy_config(void)
{
  const struct ServiceConf* svc = conf_find_service_type("nickserv");

  nick_grace = conf_service_option_int(svc, "grace_period",
                                       NICKPOLICY_GRACE_DEFAULT);

  if (nick_grace < 1 || nick_grace > NICKPOLICY_GRACE_MAX) {
    log_write(LS_SYSTEM, L_WARNING, 0,
              "nickserv: grace_period of %d is outside 1..%d; using %d",
              nick_grace, NICKPOLICY_GRACE_MAX, NICKPOLICY_GRACE_DEFAULT);
    nick_grace = NICKPOLICY_GRACE_DEFAULT;
  }
}

/** Register the hooks.
 * @param[in] mod The module's handle.
 * @return Non-zero on success.
 */
int nickpolicy_init(struct ModuleHandle* mod)
{
  timer_init(&nick_timer);

  return module_add_hook(mod, HOOK_CLIENT_REGISTERED, nick_on_registered,
                         HOOK_PRIORITY_DEFAULT, NULL)
      && module_add_hook(mod, HOOK_CLIENT_NICK_CHANGED, nick_on_nick,
                         HOOK_PRIORITY_DEFAULT, NULL)
      && module_add_hook(mod, HOOK_CLIENT_EXITING, nick_on_exiting,
                         HOOK_PRIORITY_DEFAULT, NULL);
}

/** Drop everything.  The hooks are already gone by the time this runs.
 *
 * The freezes are not lifted: a client that is frozen is one whose nick
 * nobody has proved, and unloading the service that would have asked does
 * not turn it into its own.  The core renames them instead -- see
 * account_provider_gone() -- which is what section 6 calls the safeguard.
 */
void nickpolicy_fini(void)
{
  if (nick_timer_armed) {
    timer_del(&nick_timer);
    nick_timer_armed = 0;
  }

  while (nick_holds) {
    struct NickHold* hold = nick_holds;

    nick_holds = hold->nh_next;
    MyFree(hold);
  }

  while (nick_asks) {
    struct NickAsk* ask = nick_asks;

    nick_asks = ask->na_next;
    MyFree(ask);
  }
}
