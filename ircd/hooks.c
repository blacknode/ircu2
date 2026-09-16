/*
 * IRC - Internet Relay Chat, ircd/hooks.c
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
 * @brief Dispatch of lifecycle hooks.
 */
#include "config.h"

#include "hooks.h"
#include "client.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_events.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "numeric.h"
#include "send.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** One registered hook. */
struct Hook {
  struct Hook*         h_next;      /**< Next hook in the same chain. */
  struct ModuleHandle* h_mod;       /**< Module that registered it. */
  const char*          h_owner;     /**< Its name, for logs. */
  HookFn               h_fn;        /**< The callback. */
  void*                h_user;      /**< Opaque pointer for the callback. */
  int                  h_priority;  /**< Lower runs earlier. */
  int                  h_dead;      /**< Removed while the chain was running. */
  char*                h_cmd;       /**< Command a command hook watches, or
                                         NULL for every command. */
  unsigned int         h_flags;     /**< HOOK_CMD_* flags. */
};

/** One chain per hook type. */
static struct {
  struct Hook* chain;      /**< Hooks, ordered by priority. */
  unsigned int count;      /**< Live hooks in the chain. */
  unsigned int calls;      /**< Times this hook type has run. */
} hooks[HOOK_LAST];

/** Depth of hook_run() calls currently on the stack.
 *
 * While non-zero, removing a hook only marks it dead; the chain is not
 * relinked until the outermost run finishes.  A hook that unregisters
 * itself is a normal thing to want, and it must not free the node the
 * dispatch loop is standing on.
 */
static int hook_run_depth;

/** Set when a dead hook needs reaping once dispatch unwinds. */
static int hook_reap_pending;

/** Names of the hook types, indexed by enum HookType. */
static const char* hook_names[HOOK_LAST] = {
  "CLIENT_PRE_REGISTER",
  "CLIENT_REGISTERED",
  "CLIENT_PRE_NICK",
  "CLIENT_NICK_CHANGED",
  "CLIENT_PRE_UMODE",
  "CLIENT_EXITING",
  "CHANNEL_PRE_CREATE",
  "CHANNEL_PRE_JOIN",
  "CHANNEL_JOINED",
  "CHANNEL_PRE_PART",
  "CHANNEL_PARTED",
  "CHANNEL_PRE_MODE",
  "CHANNEL_PRE_TOPIC",
  "CHANNEL_DESTROYED",
  "MESSAGE_PRE_CHANNEL",
  "MESSAGE_PRE_PRIVATE",
  "MESSAGE_RECEIVED",
  "COMMAND_PRE",
  "COMMAND_POST",
  "SERVER_LINKED",
  "SERVER_SPLIT",
  "CONFIG_LOADED"
};

/** Get the name of a hook type.
 * @param[in] type Hook type.
 * @return Its name, or "?" if the type is out of range.
 */
const char* hook_type_name(enum HookType type)
{
  if (type < 0 || type >= HOOK_LAST)
    return "?";

  return hook_names[type];
}

/** Return how many hooks are registered for a type. */
unsigned int hook_count(enum HookType type)
{
  assert(type >= 0 && type < HOOK_LAST);
  return hooks[type].count;
}

/** Return how many times a hook type has been run. */
unsigned int hook_calls(enum HookType type)
{
  assert(type >= 0 && type < HOOK_LAST);
  return hooks[type].calls;
}

/* ------------------------------------------------------------------- *
 * Suspended operations.                                               *
 *                                                                     *
 * See "Suspending a hook" in include/hooks.h for the three rules this *
 * implements and why each one is there.                               *
 * ------------------------------------------------------------------- */

/** One operation a module asked to answer later. */
struct HookPending {
  struct HookPending*  hp_next;     /**< Next hold, in no particular order. */
  hook_token_t         hp_token;    /**< What hook_resume() is called with. */
  enum HookType        hp_type;     /**< Hook point that was suspended. */
  struct ModuleHandle* hp_mod;      /**< Module that owes an answer. */
  char*                hp_owner;    /**< Its name; a copy, because the
                                         module's own storage goes away
                                         before the log message would. */
  struct Client*       hp_client;   /**< Client the operation is about. */
  time_t               hp_deadline; /**< When the hold is refused. */
  HookResumeFn         hp_done;     /**< The core's way back in. */
  void*                hp_data;     /**< Opaque pointer for hp_done. */
};

/** Outstanding holds.  Short by construction: one per operation waiting. */
static struct HookPending* hook_pending_list;

/** How many are on that list. */
static unsigned int hook_pending_num;

/** Ceiling on holds, so a module that suspends and never answers cannot
 * grow the list without bound.  Past this a suspension is refused outright
 * rather than recorded.
 */
#define HOOK_PENDING_MAX 16384

/** Last token handed out.  Never zero: zero means "cannot be suspended". */
static hook_token_t hook_last_token;

/** Timer for the earliest deadline, armed only while something is held.
 *
 * A timer of its own rather than a ride on check_pings(): that pass is
 * scheduled minutes ahead on an idle server, and a hold created just after
 * it ran would wait for the next one.  A deadline that is only enforced
 * eventually is not a deadline.  Nothing is armed while nothing is held,
 * so a server with no modules pays for none of this.
 */
static struct Timer hook_timer;

/** Whether #hook_timer is on the queue. */
static int hook_timer_armed;

/** Whether the timer struct has been through timer_init().
 *
 * Once, and never again.  timer_init() zeroes the generator's flags,
 * GEN_MARKED among them, and that flag is what tells timer_add() it is
 * being called from inside the timer's own expiry -- which is exactly
 * where it is called from, every time a callback starts something new.
 * Without the flag the timer is queued a second time while timer_run()
 * still holds it, and the server dies later on an event for a generator
 * that is no longer active.  Re-arming an initialised timer is what
 * check_pings() does.
 */
static int hook_timer_ready;

static void hook_pending_timeout(struct Event* ev);

/** Make sure the timer will fire by the earliest outstanding deadline.
 *
 * Every hold gets the same FEAT_HOOK_TIMEOUT, so a new one is always later
 * than the one the timer is already set for and there is nothing to move.
 * The other direction -- the earliest hold being answered first -- leaves
 * the timer early, which costs one callback that finds nothing to do.
 */
static void hook_pending_arm(void)
{
  time_t deadline;

  if (hook_timer_armed)
    return;

  deadline = hook_pending_deadline();
  if (!deadline)
    return;

  if (!hook_timer_ready) {
    timer_init(&hook_timer);
    hook_timer_ready = 1;
  }

  timer_add(&hook_timer, hook_pending_timeout, 0, TT_ABSOLUTE, deadline);
  hook_timer_armed = 1;
}

/** Refuse whatever has waited too long, and set the timer for the rest. */
static void hook_pending_timeout(struct Event* ev)
{
  if (ev_type(ev) != ET_EXPIRE) {
    if (ev_type(ev) == ET_DESTROY)
      hook_timer_armed = 0;
    return;
  }

  /* An absolute timer is done once it has expired; re-arming is a fresh
   * timer_add(), the same way check_pings() does it.
   */
  hook_timer_armed = 0;

  hook_pending_expire(CurrentTime);
  hook_pending_arm();
}

/** Find an outstanding hold by token. */
static struct HookPending* hook_pending_find(hook_token_t token)
{
  struct HookPending* hp;

  for (hp = hook_pending_list; hp; hp = hp->hp_next)
    if (hp->hp_token == token)
      return hp;

  return NULL;
}

/** Hand out a token that is not zero and not already outstanding.
 *
 * The counter wraps on a 32-bit unsigned long after four billion holds,
 * and a reused token would resume somebody else's operation; the list is
 * a handful of entries, so checking is free and the alternative is a bug
 * that appears once a year.
 */
static hook_token_t hook_token_new(void)
{
  do {
    ++hook_last_token;
  } while (hook_last_token == 0 || hook_pending_find(hook_last_token));

  return hook_last_token;
}

/** Unlink and free one hold.  Does not call its completion callback. */
static void hook_pending_free(struct HookPending* hp)
{
  struct HookPending** hp_p;

  for (hp_p = &hook_pending_list; *hp_p; hp_p = &(*hp_p)->hp_next) {
    if (*hp_p == hp) {
      *hp_p = hp->hp_next;
      hook_pending_num--;
      break;
    }
  }

  MyFree(hp->hp_owner);
  MyFree(hp);
}

/** Refuse one hold and tell the core.
 *
 * The hold is off the list before the callback runs: the callback usually
 * ends up in exit_client(), which comes back through
 * hook_pending_cancel(), and it must not find this entry still linked.
 */
static void hook_pending_refuse(struct HookPending* hp, const char* reason)
{
  HookResumeFn done = hp->hp_done;
  void* data = hp->hp_data;

  hook_pending_free(hp);
  (*done)(data, HOOK_DENY, reason);
}

/** Number of operations currently held by a module. */
unsigned int hook_pending_count(void)
{
  return hook_pending_num;
}

/** Earliest deadline of any outstanding hold, or 0 if there are none. */
time_t hook_pending_deadline(void)
{
  struct HookPending* hp;
  time_t earliest = 0;

  for (hp = hook_pending_list; hp; hp = hp->hp_next)
    if (!earliest || hp->hp_deadline < earliest)
      earliest = hp->hp_deadline;

  return earliest;
}

/** Refuse every hold whose deadline has passed.
 *
 * One at a time, restarting the walk after each: the completion callback
 * can exit a client, and that removes further entries from this list.
 *
 * @param[in] now Current time.
 * @return Number of holds that expired.
 */
int hook_pending_expire(time_t now)
{
  struct HookPending* hp;
  int expired = 0;

  for (;;) {
    for (hp = hook_pending_list; hp; hp = hp->hp_next)
      if (hp->hp_deadline <= now)
        break;

    if (!hp)
      break;

    log_write(LS_SYSTEM, L_ERROR, 0,
              "Module %s held %s for too long without answering; refusing it",
              hp->hp_owner, hook_type_name(hp->hp_type));

    hook_pending_refuse(hp, "Timed out waiting for a module");
    expired++;
  }

  return expired;
}

/** Drop every hold on a client, without resuming anything. */
void hook_pending_cancel(struct Client* client)
{
  struct HookPending* hp;
  struct HookPending* next;

  if (!client)
    return;

  for (hp = hook_pending_list; hp; hp = next) {
    next = hp->hp_next;
    if (hp->hp_client == client)
      hook_pending_free(hp);
  }
}

/** Refuse every hold a module owes an answer for.
 *
 * Called when it is unloaded: the callback that was going to resume the
 * operation is about to be unmapped, so the answer is never coming.
 */
static void hook_pending_drop_module(struct ModuleHandle* mod)
{
  struct HookPending* hp;

  for (;;) {
    for (hp = hook_pending_list; hp; hp = hp->hp_next)
      if (hp->hp_mod == mod)
        break;

    if (!hp)
      break;

    log_write(LS_SYSTEM, L_ERROR, 0,
              "Module %s was unloaded while holding %s; refusing it",
              hp->hp_owner, hook_type_name(hp->hp_type));

    hook_pending_refuse(hp, "The module deciding this was unloaded");
  }
}

/** Release one hook node. */
static void hook_free(struct Hook* h)
{
  MyFree(h->h_cmd);
  MyFree(h);
}

/** Free hooks marked dead during dispatch. */
static void hook_reap(void)
{
  struct Hook** h_p;
  struct Hook* h;
  enum HookType type;

  if (!hook_reap_pending)
    return;

  for (type = 0; type < HOOK_LAST; type++) {
    for (h_p = &hooks[type].chain; (h = *h_p); ) {
      if (h->h_dead) {
        *h_p = h->h_next;
        hook_free(h);
      } else
        h_p = &h->h_next;
    }
  }

  hook_reap_pending = 0;
}

/** Register a hook.  The shared half of hook_add() and
 * hook_add_command(); \a cmd and \a flags are meaningful only for the
 * command hook points.
 */
static int hook_add_full(struct ModuleHandle* mod, const char* owner,
                         enum HookType type, HookFn fn, int priority,
                         void* user, const char* cmd, unsigned int flags)
{
  struct Hook** h_p;
  struct Hook* h;

  assert(0 != fn);

  if (!owner)
    owner = "?";

  if (type < 0 || type >= HOOK_LAST) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Module %s tried to register an unknown hook type %d",
              owner, (int) type);
    return 0;
  }

  h = (struct Hook*) MyCalloc(1, sizeof(struct Hook));
  h->h_mod = mod;
  h->h_owner = owner;
  h->h_fn = fn;
  h->h_user = user;
  h->h_priority = priority;
  h->h_flags = flags;
  if (cmd)
    DupString(h->h_cmd, cmd);

  /* Insert by priority, after any hook of equal priority so that the order
   * modules were loaded in decides ties.
   */
  for (h_p = &hooks[type].chain; *h_p; h_p = &(*h_p)->h_next)
    if ((*h_p)->h_priority > priority)
      break;

  h->h_next = *h_p;
  *h_p = h;
  hooks[type].count++;

  return 1;
}

/** Register a hook.
 * @param[in] mod Module registering the hook; used only as an identity
 *   token, so hooks.c does not need to know what a module is.
 * @param[in] owner The module's name, for log messages.  Must stay valid
 *   for as long as the hook is registered.
 * @param[in] type Hook point to attach to.
 * @param[in] fn Callback to run.
 * @param[in] priority Lower numbers run earlier; ties keep insertion order.
 * @param[in] user Opaque pointer handed back to the callback.
 * @return Non-zero on success.
 */
int hook_add(struct ModuleHandle* mod, const char* owner, enum HookType type,
             HookFn fn, int priority, void* user)
{
  /* A command hook carries a command name and flags, which this entry
   * point has nowhere to put; sending one through here would register a
   * hook that fires on every command, which is never what was meant.
   */
  if (type == HOOK_COMMAND_PRE || type == HOOK_COMMAND_POST) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Module %s tried to register %s with hook_add(); "
              "command hooks go through hook_add_command()",
              owner ? owner : "?", hook_type_name(type));
    return 0;
  }

  return hook_add_full(mod, owner, type, fn, priority, user, 0, 0);
}

/** Attach a callback to a command hook.
 *
 * Naming a command is the normal case and watching every one is the
 * exception: a module that asks for all of them is handed every line the
 * server parses, including the server-to-server traffic.
 *
 * @param[in] mod Module registering the hook.
 * @param[in] owner The module's name, for log messages.
 * @param[in] type #HOOK_COMMAND_PRE or #HOOK_COMMAND_POST.
 * @param[in] cmd Command to watch, or NULL for every command.
 * @param[in] fn Callback to run.
 * @param[in] priority Lower numbers run earlier.
 * @param[in] user Opaque pointer handed back to the callback.
 * @param[in] flags Bitwise combination of HOOK_CMD_* values.
 * @return Non-zero on success.
 */
int hook_add_command(struct ModuleHandle* mod, const char* owner,
                     enum HookType type, const char* cmd, HookFn fn,
                     int priority, void* user, unsigned int flags)
{
  if (type != HOOK_COMMAND_PRE && type != HOOK_COMMAND_POST) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Module %s tried to register %s as a command hook",
              owner ? owner : "?", hook_type_name(type));
    return 0;
  }

  if (cmd && EmptyString(cmd))
    cmd = 0;

  return hook_add_full(mod, owner, type, fn, priority, user, cmd, flags);
}

/** Detach a command hook.
 * @param[in] mod Module that owns it.
 * @param[in] type Hook point it was attached to.
 * @param[in] cmd Command it was attached for, or NULL if it watched all.
 * @param[in] fn The callback.
 * @return Non-zero if it was found and detached.
 */
int hook_del_command(struct ModuleHandle* mod, enum HookType type,
                     const char* cmd, HookFn fn)
{
  struct Hook** h_p;
  struct Hook* h;

  if (type != HOOK_COMMAND_PRE && type != HOOK_COMMAND_POST)
    return 0;

  if (cmd && EmptyString(cmd))
    cmd = 0;

  for (h_p = &hooks[type].chain; (h = *h_p); h_p = &h->h_next) {
    if (h->h_fn != fn || h->h_mod != mod || h->h_dead)
      continue;
    /* The same callback may watch two commands; the name is part of what
     * identifies the registration.
     */
    if (!cmd != !h->h_cmd)
      continue;
    if (cmd && ircd_strcmp(cmd, h->h_cmd))
      continue;

    hooks[type].count--;

    if (hook_run_depth) {
      h->h_dead = 1;
      hook_reap_pending = 1;
    } else {
      *h_p = h->h_next;
      hook_free(h);
    }

    return 1;
  }

  return 0;
}

/** Remove a hook a module registered.
 * @param[in] mod Module that owns the hook.
 * @param[in] type Hook point it was attached to.
 * @param[in] fn The callback to remove.
 * @return Non-zero if a hook was found and removed.
 */
int hook_del(struct ModuleHandle* mod, enum HookType type, HookFn fn)
{
  struct Hook** h_p;
  struct Hook* h;

  if (type < 0 || type >= HOOK_LAST)
    return 0;

  for (h_p = &hooks[type].chain; (h = *h_p); h_p = &h->h_next) {
    if (h->h_fn != fn || h->h_mod != mod || h->h_dead)
      continue;

    hooks[type].count--;

    if (hook_run_depth) {
      /* The dispatch loop may be standing on this node; leave it linked
       * and reap it when the outermost run returns.
       */
      h->h_dead = 1;
      hook_reap_pending = 1;
    } else {
      *h_p = h->h_next;
      hook_free(h);
    }

    return 1;
  }

  return 0;
}

/** Remove every hook a module registered.
 * @param[in] mod Module being torn down.
 */
void hook_del_module(struct ModuleHandle* mod)
{
  struct Hook** h_p;
  struct Hook* h;
  enum HookType type;

  /* First the operations it was holding: the callback that would have
   * resumed them is in the code about to be unmapped.
   */
  hook_pending_drop_module(mod);

  for (type = 0; type < HOOK_LAST; type++) {
    for (h_p = &hooks[type].chain; (h = *h_p); ) {
      if (h->h_mod != mod) {
        h_p = &h->h_next;
        continue;
      }

      if (!h->h_dead)
        hooks[type].count--;

      if (hook_run_depth) {
        h->h_dead = 1;
        hook_reap_pending = 1;
        h_p = &h->h_next;
      } else {
        *h_p = h->h_next;
        hook_free(h);
      }
    }
  }
}

/** Prepare a hook context for use.
 * @param[out] ctx Context to initialise.
 */
void hook_context_init(struct HookContext* ctx)
{
  assert(0 != ctx);
  memset(ctx, 0, sizeof(*ctx));
}

/** Run every hook registered for a type until one decides.
 *
 * The chain stops at the first result other than #HOOK_CONTINUE, and that
 * result is what the caller sees.  A #HOOK_DENY cannot be overturned by a
 * later hook: if it could, the outcome would depend on module load order,
 * which is not something anyone should have to debug.
 *
 * @param[in] type Hook point to run.
 * @param[in,out] ctx Context describing the operation.
 * @return The deciding result, or #HOOK_CONTINUE if nobody decided.
 */
enum HookResult hook_run(enum HookType type, struct HookContext* ctx)
{
  enum HookResult result = HOOK_CONTINUE;
  struct Hook* h;

  assert(type >= 0 && type < HOOK_LAST);
  assert(0 != ctx);

  hooks[type].calls++;

  hook_run_depth++;

  for (h = hooks[type].chain; h; h = h->h_next) {
    if (h->h_dead)
      continue;

    result = (*h->h_fn)(ctx, h->h_user);

    if (result == HOOK_PENDING) {
      /* This call site has no way back in -- which the module could have
       * seen from HookContext::hc_token being zero.  Carry on rather than
       * stall an operation that could never be resumed.
       */
      log_write(LS_SYSTEM, L_ERROR, 0,
                "Module %s returned HOOK_PENDING from %s, which cannot be "
                "suspended; treating it as HOOK_CONTINUE",
                h->h_owner, hook_type_name(type));
      result = HOOK_CONTINUE;
    }

    if (result != HOOK_CONTINUE)
      break;
  }

  hook_run_depth--;

  if (!hook_run_depth)
    hook_reap();

  return result;
}

/** Run a hook chain that a module may hold.
 *
 * The token is minted before the chain runs, because a module has to read
 * it out of the context on its way to returning #HOOK_PENDING: the answer
 * comes back long after this function has returned and the context, which
 * is the caller's stack, is gone.  If nobody suspends, the token is simply
 * never used.
 *
 * @param[in] type Hook point to run.
 * @param[in,out] ctx Context describing the operation.
 * @param[in] client Client the operation is about, or NULL.
 * @param[in] done Called when the answer arrives, or when the hold ends
 *   for any other reason.
 * @param[in] data Opaque pointer for \a done.
 * @param[in] deadline Absolute time after which the hold is refused.
 * @return #HOOK_PENDING if the operation was suspended, otherwise what
 *   hook_run() would have returned.
 */
enum HookResult hook_run_suspendable(enum HookType type,
                                     struct HookContext* ctx,
                                     struct Client* client,
                                     HookResumeFn done, void* data,
                                     time_t deadline)
{
  enum HookResult result = HOOK_CONTINUE;
  struct HookPending* hp;
  struct Hook* held = NULL;
  struct Hook* h;
  hook_token_t token;

  assert(type >= 0 && type < HOOK_LAST);
  assert(0 != ctx);
  assert(0 != done);

  if (!hooks[type].count)
    return HOOK_CONTINUE;

  token = hook_token_new();
  ctx->hc_token = token;

  hooks[type].calls++;
  hook_run_depth++;

  for (h = hooks[type].chain; h; h = h->h_next) {
    if (h->h_dead)
      continue;

    result = (*h->h_fn)(ctx, h->h_user);

    if (result == HOOK_PENDING) {
      held = h;
      break;
    }

    if (result != HOOK_CONTINUE)
      break;
  }

  hook_run_depth--;

  if (!hook_run_depth)
    hook_reap();

  /* The context outlives this call only in the caller's frame, and a token
   * there would invite a resume for an operation that was never held.
   */
  ctx->hc_token = 0;

  if (result != HOOK_PENDING)
    return result;

  if (hook_pending_num >= HOOK_PENDING_MAX) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Module %s suspended %s with %u operations already held; "
              "refusing it", held->h_owner, hook_type_name(type),
              hook_pending_num);
    return HOOK_DENY;
  }

  hp = (struct HookPending*) MyCalloc(1, sizeof(struct HookPending));
  hp->hp_token = token;
  hp->hp_type = type;
  hp->hp_mod = held->h_mod;
  DupString(hp->hp_owner, held->h_owner);
  hp->hp_client = client;
  hp->hp_deadline = deadline;
  hp->hp_done = done;
  hp->hp_data = data;

  hp->hp_next = hook_pending_list;
  hook_pending_list = hp;
  hook_pending_num++;

  hook_pending_arm();

  return HOOK_PENDING;
}

/** Answer a suspended hook.
 *
 * @param[in] token The value the module read from HookContext::hc_token.
 * @param[in] result #HOOK_DENY to refuse, anything else to proceed.
 * @param[in] reason Text explaining a refusal, or NULL.
 * @return Non-zero if the token was outstanding.
 */
int hook_resume(hook_token_t token, enum HookResult result,
                const char* reason)
{
  struct HookPending* hp;
  HookResumeFn done;
  void* data;

  hp = hook_pending_find(token);

  if (!hp) {
    /* Not an error the server can do anything about, and not one it can
     * hide either: the operation this was meant to answer has already
     * been decided -- the client left, the deadline passed, or the module
     * answered twice.
     */
    log_write(LS_SYSTEM, L_INFO, 0,
              "hook_resume() for token %lu, which is no longer outstanding",
              (unsigned long) token);
    return 0;
  }

  /* Off the list before the callback runs; see hook_pending_refuse(). */
  done = hp->hp_done;
  data = hp->hp_data;
  hook_pending_free(hp);

  (*done)(data, result, reason);

  return 1;
}

/** Run a notification hook.
 *
 * The early return is what keeps these call sites free on a server with no
 * modules loaded: one comparison, no context to build.
 *
 * @param[in] type Hook point to run.
 * @param[in] client Client the event is about.
 * @param[in] source Origin of the event, or NULL.
 * @param[in] chan Channel involved, or NULL.
 * @param[in] arg Extra detail, or NULL.
 */
void hook_notify(enum HookType type, struct Client* client,
                 struct Client* source, struct Channel* chan,
                 const char* arg)
{
  struct HookContext ctx;

  if (!hook_is_active(type))
    return;

  hook_context_init(&ctx);
  ctx.hc_client = client;
  ctx.hc_source = source ? source : client;
  ctx.hc_channel = chan;
  ctx.hc_arg = arg;

  hook_run(type, &ctx);
}

/** Return non-zero if anything is listening at a command hook point.
 *
 * This is the whole cost of the command hooks on a server where no module
 * uses them: one array read and a comparison, per line parsed.
 */
int hook_command_active(enum HookType type)
{
  if (type != HOOK_COMMAND_PRE && type != HOOK_COMMAND_POST)
    return 0;

  return hooks[type].count != 0;
}

/** Depth of hook_run_command() calls on the stack.
 *
 * A hook that makes the server dispatch another command is a reasonable
 * thing to write -- a module that answers a KICK with a MODE -- and a
 * module that does it without noticing it has written a loop is just as
 * easy to write.  The depth bounds it.
 */
static int hook_command_depth;

/** Deepest nesting of command dispatch a hook may cause. */
#define HOOK_COMMAND_MAX_DEPTH 8

/** Run the command hooks for one dispatch.
 *
 * The context arrives filled in: parse.c owns the message table and knows
 * how to turn a command's declared subject into a client and a channel, so
 * it does that once and this only dispatches.  See hooks.h for the rules
 * enforced here and why each one is there.
 *
 * @param[in] type #HOOK_COMMAND_PRE or #HOOK_COMMAND_POST.
 * @param[in,out] ctx Context, with HookContext::hc_command set.
 * @return #HOOK_DENY if the command must not run, #HOOK_CONTINUE otherwise.
 */
enum HookResult hook_run_command(enum HookType type, struct HookContext* ctx)
{
  enum HookResult res = HOOK_CONTINUE;
  struct Hook* h;
  const char* cmd;
  int from_service;

  assert(type == HOOK_COMMAND_PRE || type == HOOK_COMMAND_POST);
  assert(0 != ctx);
  assert(0 != ctx->hc_command);

  if (!hooks[type].count)
    return HOOK_CONTINUE;

  cmd = ctx->hc_command->hcc_cmd;

  /* A hook that got the server dispatching commands in a circle is a bug
   * in that module, but it is this server that would run out of stack.
   */
  if (hook_command_depth >= HOOK_COMMAND_MAX_DEPTH) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Command hooks nested %d deep at %s; not running them again",
              hook_command_depth, cmd);
    return HOOK_CONTINUE;
  }

  /* A service bot acts for the server.  Most modules should not see that
   * at all, so it is opt-in per hook rather than a decision made here.
   */
  from_service = ctx->hc_source && IsServiceBot(ctx->hc_source);

  hooks[type].calls++;

  hook_command_depth++;
  hook_run_depth++;

  for (h = hooks[type].chain; h; h = h->h_next) {
    if (h->h_dead)
      continue;
    if (h->h_cmd && ircd_strcmp(h->h_cmd, cmd))
      continue;
    if (from_service && !(h->h_flags & HOOK_CMD_INCLUDE_SERVICES))
      continue;

    res = (*h->h_fn)(ctx, h->h_user);

    if (res == HOOK_PENDING) {
      /* A command is dispatched, handled and finished before the next line
       * is read; holding one would mean holding the line that produced it,
       * and re-entering a handler halfway through is not something the
       * server can do.  See hooks.h.
       */
      log_write(LS_SYSTEM, L_ERROR, 0,
                "Module %s returned HOOK_PENDING from %s, which cannot be "
                "suspended; treating it as HOOK_CONTINUE",
                h->h_owner, hook_type_name(type));
      res = HOOK_CONTINUE;
    }

    if (res != HOOK_CONTINUE)
      break;
  }

  hook_run_depth--;
  hook_command_depth--;

  if (!hook_run_depth)
    hook_reap();

  if (res != HOOK_DENY)
    return HOOK_CONTINUE;

  /* Only a command from a client of this server can be refused.  The same
   * command reaching another server has already been applied there, so
   * refusing it here would leave this server disagreeing with the network
   * about who is on what channel -- a worse outcome than the one the
   * module was trying to prevent.
   */
  if (type != HOOK_COMMAND_PRE || !ctx->hc_source
      || !MyConnect(ctx->hc_source)) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "A module denied %s from %s at %s, which is not a client of "
              "this server; ignoring the veto",
              cmd, ctx->hc_source ? cli_name(ctx->hc_source) : "?",
              hook_type_name(type));
    return HOOK_CONTINUE;
  }

  return HOOK_DENY;
}

/** Tell a client why a hook refused an operation.
 * @param[in] to Client to answer.
 * @param[in] ctx Context the hook filled in.
 * @param[in] numeric Numeric to use when the hook did not choose one.
 * @param[in] arg Parameter for the numeric.
 */
void hook_deny_reply(struct Client* to, const struct HookContext* ctx,
                     int numeric, const char* arg)
{
  int num;

  assert(0 != to);
  assert(0 != ctx);

  num = ctx->hc_numeric ? ctx->hc_numeric : numeric;

  /* Always SND_EXPLICIT, never the numeric's own format.  Numerics do not
   * share a parameter shape -- ERR_CANNOTSENDTOCHAN takes "%s", but
   * ERR_UMODEUNKNOWNFLAG takes "%c" and ERR_NOPRIVILEGES takes nothing --
   * and a helper that guesses would hand a string to a "%c" the first time
   * someone reached for a numeric it had not been tested with.  Supplying
   * the whole format here means the shape is right whatever numeric a
   * module picks.
   */
  send_reply(to, SND_EXPLICIT | num, "%s :%s", arg ? arg : "*",
             ctx->hc_reason[0] ? ctx->hc_reason : "Refused by a module");
}

/** Initialise the hook subsystem, discarding anything already registered.
 *
 * Freeing first rather than just zeroing the table matters: an init that
 * silently leaks when called twice is a trap for anyone who reaches for it
 * as a reset, and the tests do exactly that between cases.
 */
void hooks_init(void)
{
  struct Hook* h;
  struct Hook* next;
  enum HookType type;

  for (type = 0; type < HOOK_LAST; type++) {
    for (h = hooks[type].chain; h; h = next) {
      next = h->h_next;
      MyFree(h);
    }
  }

  while (hook_pending_list) {
    struct HookPending* hp = hook_pending_list;
    hook_pending_list = hp->hp_next;
    MyFree(hp->hp_owner);
    MyFree(hp);
  }

  if (hook_timer_armed) {
    timer_del(&hook_timer);
    hook_timer_armed = 0;
  }

  memset(hooks, 0, sizeof(hooks));
  hook_run_depth = 0;
  hook_reap_pending = 0;
  hook_pending_num = 0;
}
