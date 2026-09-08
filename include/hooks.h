#ifndef INCLUDED_hooks_h
#define INCLUDED_hooks_h
/*
 * IRC - Internet Relay Chat, include/hooks.h
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
 * @brief Lifecycle hooks that modules can attach to.
 *
 * A hook lets a module observe, and in some cases veto, a step in the life
 * of a user or a channel.  The set of hook points is closed: a module
 * chooses from this list rather than inventing its own, so every point is
 * one the server documents and guarantees.
 *
 * Hooks run in the main thread, in the middle of handling a client's
 * command.  A slow hook delays every other client on the server.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif
#ifndef INCLUDED_ircd_defs_h
#include "ircd_defs.h"
#endif

struct Client;
struct Channel;
struct ModuleHandle;

/** Points in the lifecycle a module can hook.
 *
 * Points named PRE_ run before the server acts and can veto; the rest are
 * notifications, delivered after the fact, whose return value is ignored.
 */
enum HookType {
  /* --- client lifecycle --- */
  HOOK_CLIENT_PRE_REGISTER,   /**< Veto: may this client finish registering? */
  HOOK_CLIENT_REGISTERED,     /**< A local client finished registering. */
  HOOK_CLIENT_PRE_NICK,       /**< Veto: may this client use this nick? */
  HOOK_CLIENT_NICK_CHANGED,   /**< A client changed nick. */
  HOOK_CLIENT_PRE_UMODE,      /**< Veto: may this client set these modes? */
  HOOK_CLIENT_EXITING,        /**< A client is leaving. */

  /* --- channel lifecycle --- */
  HOOK_CHANNEL_PRE_CREATE,    /**< Veto: may this client create this channel? */
  HOOK_CHANNEL_PRE_JOIN,      /**< Veto: may this client join? */
  HOOK_CHANNEL_JOINED,        /**< A client joined a channel. */
  HOOK_CHANNEL_PRE_PART,      /**< Veto: may this client part? */
  HOOK_CHANNEL_PARTED,        /**< A client left a channel. */
  HOOK_CHANNEL_PRE_MODE,      /**< Veto: may this mode change happen? */
  HOOK_CHANNEL_PRE_TOPIC,     /**< Veto: may this topic be set? */
  HOOK_CHANNEL_DESTROYED,     /**< A channel became empty and went away. */

  /* --- messaging --- */
  HOOK_MESSAGE_PRE_CHANNEL,   /**< Veto or rewrite a message to a channel. */
  HOOK_MESSAGE_PRE_PRIVATE,   /**< Veto or rewrite a message to a user. */

  /* --- network --- */
  HOOK_SERVER_LINKED,         /**< A server finished linking. */
  HOOK_SERVER_SPLIT,          /**< A server left the network. */

  HOOK_LAST                   /**< Number of hook types. */
};

/** What a hook decided. */
enum HookResult {
  HOOK_CONTINUE,   /**< No opinion; carry on down the chain. */
  HOOK_ALLOW,      /**< Permit, and stop the chain. */
  HOOK_DENY,       /**< Refuse, and stop the chain.  The server aborts. */

  /* Reserved for a future asynchronous hook: a module returning this would
   * be saying "hold this operation, I will answer later".  It is not
   * implemented -- returning it today is treated as HOOK_CONTINUE and
   * logged -- but the value is spoken for so that adding asynchronous
   * hooks later does not renumber this enum and break every module built
   * against it.
   */
  HOOK_PENDING     /**< Reserved; not yet implemented. */
};

/** Everything a hook is told about the operation it is judging.
 *
 * Which fields are meaningful depends on the hook; each one is documented
 * in doc/readme.modules.  Fields that do not apply are NULL or empty.
 */
struct HookContext {
  struct Client*  hc_client;    /**< Client the action is about. */
  struct Client*  hc_source;    /**< Origin of the action, if different. */
  struct Channel* hc_channel;   /**< Channel involved, if any. */

  const char*     hc_arg;       /**< Proposed nick, text, mode string, ... */

  /** Buffer for a rewritten value, or NULL if this hook does not rewrite.
   *
   * The server owns the buffer.  A module writes into it and sets
   * #hc_rewritten; it must never store a pointer of its own here.
   */
  char*           hc_rewrite;
  size_t          hc_rewrite_len;  /**< Size of #hc_rewrite in bytes. */
  int             hc_rewritten;    /**< Set by a module that rewrote. */

  /** Numeric to send when denying, or 0 to let the server pick. */
  int             hc_numeric;
  /** Reason to send when denying. */
  char            hc_reason[TOPICLEN + 1];
};

/** A hook callback.
 * @param[in,out] ctx What the hook is judging.
 * @param[in] user Opaque pointer supplied at registration.
 * @return What the hook decided.
 */
typedef enum HookResult (*HookFn)(struct HookContext* ctx, void* user);

/** Default priority for a hook that does not care about ordering. */
#define HOOK_PRIORITY_DEFAULT 100

/*
 * Registration.  Modules call these through their handle so the server can
 * revert everything when the module is unloaded.
 */
/* mod is an opaque identity token here: hooks.c never dereferences it, so
 * this layer does not depend on the module loader.  owner is the module's
 * name, used only in log messages.
 */
extern int hook_add(struct ModuleHandle* mod, const char* owner,
                    enum HookType type, HookFn fn, int priority, void* user);
extern int hook_del(struct ModuleHandle* mod, enum HookType type, HookFn fn);
extern void hook_del_module(struct ModuleHandle* mod);

/** Number of hooks registered for a type. */
extern unsigned int hook_count(enum HookType type);

/** Number of times a hook type has been run. */
extern unsigned int hook_calls(enum HookType type);

/** Human-readable name of a hook type, for /STATS and logs. */
extern const char* hook_type_name(enum HookType type);

/*
 * Running hooks.  Called from the core, never from modules.
 */

/** Initialise a context with everything zeroed and no rewrite buffer. */
extern void hook_context_init(struct HookContext* ctx);

/** Run the chain for a hook type.
 * @param[in] type Hook to run.
 * @param[in,out] ctx Context describing the operation.
 * @return The first result that was not HOOK_CONTINUE, or HOOK_CONTINUE.
 */
extern enum HookResult hook_run(enum HookType type, struct HookContext* ctx);

/** Run a notification hook.
 *
 * Notifications have no veto and no rewrite, so their call sites do not
 * need to build a context by hand.  Does nothing, cheaply, when no module
 * is listening.
 *
 * @param[in] type Hook point to run.
 * @param[in] client Client the event is about.
 * @param[in] source Origin of the event, or NULL if the same as \a client.
 * @param[in] chan Channel involved, or NULL.
 * @param[in] arg Extra detail for the hook, or NULL.
 */
extern void hook_notify(enum HookType type, struct Client* client,
                        struct Client* source, struct Channel* chan,
                        const char* arg);

/** Non-zero if any module has registered for this hook.
 *
 * Call sites use this to skip building a context when nothing is listening,
 * which is the normal case on a server with no modules loaded.
 */
#define hook_is_active(type) (hook_count(type) > 0)

extern void hooks_init(void);

#endif /* INCLUDED_hooks_h */
