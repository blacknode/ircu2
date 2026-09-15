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
#ifndef INCLUDED_ircd_handler_h
#include "ircd_handler.h"
#endif

struct Client;
struct Channel;
struct ModuleHandle;
struct Message;

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
  HOOK_MESSAGE_RECEIVED,      /**< A service bot (+S) of this server was sent
                                   a message; see include/bot.h. */

  /* --- network --- */
  HOOK_SERVER_LINKED,         /**< A server finished linking. */
  HOOK_SERVER_SPLIT,          /**< A server left the network. */

  /* --- any command --- */
  HOOK_COMMAND_PRE,           /**< Veto: may this command run?  See below. */
  HOOK_COMMAND_POST,          /**< A command finished running. */

  /* --- the server itself --- */
  HOOK_CONFIG_LOADED,         /**< The configuration file was read in full:
                                   once at start-up, with the server ready,
                                   and again after every rehash. */

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

/** What a command hook is told about the command being dispatched.
 *
 * Reached through HookContext::hc_command, which is NULL for every hook
 * point other than #HOOK_COMMAND_PRE and #HOOK_COMMAND_POST.
 *
 * The parameters are the parser's own, and they are read-only: rewriting
 * an arbitrary parameter of a command in flight is not something the
 * server can bound the consequences of.  What a module may change is in
 * the context -- a reason when it denies, and nothing else.
 */
struct HookCommand {
  const char*      hcc_cmd;     /**< Command name, e.g. "KICK". */
  const char*      hcc_tok;     /**< Its P10 token, e.g. "K". */
  enum HandlerType hcc_handler; /**< Which handler runs, or ran. */
  int              hcc_parc;    /**< Number of parameters. */
  char* const*     hcc_parv;    /**< The parameters; parv[0] is the source. */
  int              hcc_result;  /**< #HOOK_COMMAND_POST only: what the
                                     handler returned. */
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
  int             hc_notice;    /**< For a message: non-zero if it is a NOTICE
                                     rather than a PRIVMSG. */

  /** Buffer for a rewritten value, or NULL if this hook does not rewrite.
   *
   * The server owns the buffer.  A module writes into it and sets
   * #hc_rewritten; it must never store a pointer of its own here.
   */
  char*           hc_rewrite;
  size_t          hc_rewrite_len;  /**< Size of #hc_rewrite in bytes. */
  int             hc_rewritten;    /**< Set by a module that rewrote. */

  /** The command being dispatched, for #HOOK_COMMAND_PRE and
   * #HOOK_COMMAND_POST; NULL at every other hook point.
   */
  const struct HookCommand* hc_command;

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

/*
 * Command hooks.
 *
 * #HOOK_COMMAND_PRE and #HOOK_COMMAND_POST are the two points every
 * command passes through, so that a module can see one user act on
 * another -- KICK, KILL, WHOIS, INVITE, MODE, GLINE, SLINE, JUPE -- without
 * a hook per command and without the core growing one every time a command
 * is added.  They are registered through hook_add_command() rather than
 * hook_add(), because a hook on every command is rarely what is wanted:
 * naming one keeps the rest of the traffic out of the module.
 *
 * Five rules, and the reason for each:
 *
 *  - A command whose source is a service bot (+S) reaches neither point.
 *    A service acts on the server's behalf; auditing or vetoing it is
 *    auditing the server.  A module that does want to see them -- an audit
 *    log, say -- asks with #HOOK_CMD_INCLUDE_SERVICES.
 *
 *  - #HOOK_DENY at #HOOK_COMMAND_PRE is honoured only when the source is a
 *    client of this server.  For a command that arrived from another
 *    server the point is a notification: the rest of the network has
 *    already applied it, and refusing it here would desynchronise this
 *    server rather than prevent anything.
 *
 *  - #HOOK_COMMAND_POST does not run when the handler returned
 *    @c CPTR_KILLED.  The client is gone and the pointers in the context
 *    are freed memory; a module learns about the exit from
 *    #HOOK_CLIENT_EXITING, which is where it belongs.
 *
 *  - HookCommand::hcc_parv is read-only.  See #HookCommand.
 *
 *  - A hook that causes another command to be dispatched is bounded: past
 *    a small depth the server refuses to recurse and logs it.
 */

/** Flags for hook_add_command(). */
#define HOOK_CMD_INCLUDE_SERVICES 0x0001 /**< Also see commands from a +S
                                              service bot. */

/** Attach a callback to a command hook.
 * @param[in] mod Module registering the hook.
 * @param[in] owner The module's name, for log messages.
 * @param[in] type #HOOK_COMMAND_PRE or #HOOK_COMMAND_POST.
 * @param[in] cmd Command to watch, e.g. "KICK", or NULL for every command.
 *   Matched case-insensitively against the command's name, never its P10
 *   token: a module names the command, not the wire encoding.
 * @param[in] fn Callback to run.
 * @param[in] priority Lower numbers run earlier.
 * @param[in] user Opaque pointer handed back to the callback.
 * @param[in] flags Bitwise combination of HOOK_CMD_* values, or 0.
 * @return Non-zero on success.
 */
extern int hook_add_command(struct ModuleHandle* mod, const char* owner,
                            enum HookType type, const char* cmd, HookFn fn,
                            int priority, void* user, unsigned int flags);

/** Detach a command hook.
 * @param[in] mod Module that owns it.
 * @param[in] type Hook point it was attached to.
 * @param[in] cmd Command it was attached for, or NULL if it watched all.
 * @param[in] fn The callback.
 * @return Non-zero if it was found and detached.
 */
extern int hook_del_command(struct ModuleHandle* mod, enum HookType type,
                            const char* cmd, HookFn fn);
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

/** Run the command hooks for one dispatch.
 *
 * Called from ircd/parse.c, around the handler, and from nowhere else.
 * The context arrives filled in, HookContext::hc_command included: the
 * parser owns the message table and resolves the command's declared
 * subject, so this only dispatches.
 *
 * @param[in] type #HOOK_COMMAND_PRE or #HOOK_COMMAND_POST.
 * @param[in,out] ctx Context, with HookContext::hc_command set.
 * @return #HOOK_DENY if the command must not run, #HOOK_CONTINUE otherwise.
 */
extern enum HookResult hook_run_command(enum HookType type,
                                        struct HookContext* ctx);

/** Return non-zero if anything is listening at a command hook point.
 *
 * The gate in front of hook_run_command(); parse.c uses it to keep the
 * dispatch path free on a server where no module watches commands.
 */
extern int hook_command_active(enum HookType type);

/** Tell a client why a hook refused an operation.
 *
 * ircu numerics have fixed wording with a slot for a parameter, so a
 * module's own explanation cannot simply be passed as that parameter --
 * it would land where the nick or channel belongs and the wording would
 * be the numeric's, not the module's.  Nor do numerics share a parameter
 * shape, so the format is always supplied here rather than taken from the
 * numeric.  A module that sets no reason gets a generic one.
 *
 * @param[in] to Client to answer.
 * @param[in] ctx Context the hook filled in.
 * @param[in] numeric Numeric to use when the hook did not choose one.
 * @param[in] arg Parameter for the numeric, e.g. the nick or channel.
 */
extern void hook_deny_reply(struct Client* to, const struct HookContext* ctx,
                            int numeric, const char* arg);

/** Non-zero if any module has registered for this hook.
 *
 * Call sites use this to skip building a context when nothing is listening,
 * which is the normal case on a server with no modules loaded.
 */
#define hook_is_active(type) (hook_count(type) > 0)

extern void hooks_init(void);

#endif /* INCLUDED_hooks_h */
