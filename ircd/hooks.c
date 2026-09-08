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
#include "ircd_alloc.h"
#include "ircd_log.h"

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
  "SERVER_LINKED",
  "SERVER_SPLIT"
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
        MyFree(h);
      } else
        h_p = &h->h_next;
    }
  }

  hook_reap_pending = 0;
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
      MyFree(h);
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
        MyFree(h);
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
      /* Reserved but not implemented.  Treat it as no opinion rather than
       * stalling an operation the server has no way to resume.
       */
      log_write(LS_SYSTEM, L_ERROR, 0,
                "Module %s returned HOOK_PENDING from %s, which this server "
                "does not implement; treating it as HOOK_CONTINUE",
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

  memset(hooks, 0, sizeof(hooks));
  hook_run_depth = 0;
  hook_reap_pending = 0;
}
