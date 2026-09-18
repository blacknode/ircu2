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
  HOOK_MESSAGE_DELIVERED,     /**< A message was delivered on this server,
                                   whatever server it came from.  See
                                   #HookMessage below. */

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

  /* "Hold this operation, I will answer later."  Honoured only where the
   * server has a way back in, which a module recognises by
   * HookContext::hc_token being non-zero; everywhere else it is logged and
   * treated as HOOK_CONTINUE, because stalling an operation that can never
   * be resumed would hang the client forever.  See "Suspending a hook"
   * below.
   */
  HOOK_PENDING     /**< Suspend; answer later with hook_resume(). */
};

/** What hook_resume() is called with.  Zero is never a valid token. */
typedef unsigned long hook_token_t;

/** Which command a delivered message arrived as. */
enum HookMsgKind {
  HOOK_MSG_PRIVMSG,  /**< PRIVMSG: what a user says. */
  HOOK_MSG_NOTICE,   /**< NOTICE: what a user or a service says without
                          inviting a reply. */
  HOOK_MSG_TAGMSG    /**< TAGMSG: tags with no text of their own. */
};

/** What #HOOK_MESSAGE_DELIVERED is told about the message.
 *
 * Reached through HookContext::hc_message, which is NULL at every other
 * hook point.  The rest of the context says who and where:
 * HookContext::hc_source is who sent it, HookContext::hc_client is the
 * user it was addressed to (NULL for a channel), HookContext::hc_channel
 * is the channel (NULL for a private message) and HookContext::hc_arg is
 * the text, which is the empty string for a TAGMSG.
 *
 * The point fires **wherever this server relays the message**, from a
 * local client or from a link, which is what separates it from
 * #HOOK_MESSAGE_PRE_CHANNEL and #HOOK_MESSAGE_PRE_PRIVATE: those two see
 * only what started here (see doc/proposals/006 §3.5).  A channel message
 * therefore fires once on every server carrying the channel, and a
 * private message once on each end when the two users are apart.  A
 * module that stores messages deduplicates on #hmm_msgid rather than
 * assuming it is told once -- and for that to work across a network,
 * `NETWORK_FEATURES` has to be on, because that is what carries the
 * identifier over P10.
 *
 * It is a notification: the return value is ignored, and by the time it
 * runs the message has already gone out.
 */
struct HookMessage {
  enum HookMsgKind hmm_kind;    /**< PRIVMSG, NOTICE or TAGMSG. */
  const char*      hmm_msgid;   /**< The message's network-wide name, or
                                     NULL if it has none. */
  const char*      hmm_time;    /**< When it was sent, ISO 8601 with
                                     milliseconds: the `time` tag it
                                     arrived with, so every server records
                                     the same instant, or this server's
                                     clock when it started here. */
  const char*      hmm_target;  /**< Channel or nickname as the sender
                                     addressed it. */
  int              hmm_remote;  /**< Non-zero if the sender is not a user
                                     of this server. */
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

  /** A second body, for recipients that do *not* have #hc_alt_cap.
   *
   * One message, delivered two ways.  A module that gives a message a
   * content type -- rich text is the one this exists for -- has to be
   * able to say what the message looks like to a client that never
   * agreed to that type, because the alternative is a network split
   * between the clients that understand the new thing and the clients
   * that see its markup.
   *
   * The module writes the alternative into #hc_alt, names the capability
   * in #hc_alt_cap and sets #hc_alt_set.  The relay then sends
   * HookContext::hc_arg (rewritten or not) to the clients that have the
   * capability and over the links, and #hc_alt to everybody else.  What
   * a hook that stores messages is told is the **alternative**: a
   * transcript is read back by whoever reads it, and the one body it can
   * keep is the one everybody can read.
   *
   * The server owns the buffer; a module writes into it and must never
   * store a pointer of its own here.
   */
  char*           hc_alt;
  size_t          hc_alt_len;      /**< Size of #hc_alt in bytes. */
  int             hc_alt_cap;      /**< Capability position, or CAP_NONE. */
  int             hc_alt_set;      /**< Set by a module that filled it in. */
  /** A client tag not to relay with #hc_alt, with its @c + , or NULL.
   *
   * The tag that says what the rich body is would be a lie on the other
   * one.  The module names it because the core never learned what it
   * means.
   */
  const char*     hc_alt_tag;

  /** The command being dispatched, for #HOOK_COMMAND_PRE and
   * #HOOK_COMMAND_POST; NULL at every other hook point.
   */
  const struct HookCommand* hc_command;

  /** The message delivered, for #HOOK_MESSAGE_DELIVERED; NULL at every
   * other hook point.
   */
  const struct HookMessage* hc_message;

  /** Token for hook_resume(), or 0 if this point cannot be suspended.
   *
   * A module reads it *before* returning #HOOK_PENDING and keeps it; the
   * context is the caller's stack and is gone by the time the answer
   * arrives, which is why the reason travels as an argument of
   * hook_resume() rather than in HookContext::hc_reason.
   */
  hook_token_t    hc_token;

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

/*
 * Suspending a hook.
 *
 * A module that has to ask something slow -- a database, a password hash on
 * a worker thread, a process of its own -- cannot answer from inside the
 * hook: the main thread is the one thread that touches core state, and a
 * blocking call there stalls every other client on the server.  Such a
 * module reads HookContext::hc_token, returns #HOOK_PENDING, and calls
 * hook_resume() with that token when it knows the answer.
 *
 * Three rules, and the reason for each:
 *
 *  - Only a call site that passed hook_run_suspendable() can be held, and a
 *    module knows which those are: hc_token is non-zero exactly there.  A
 *    #HOOK_PENDING anywhere else is logged and read as #HOOK_CONTINUE.  The
 *    server cannot invent a way to re-enter an operation it has already
 *    half-applied.
 *
 *  - Suspending stops the chain, like #HOOK_ALLOW and #HOOK_DENY do.  The
 *    module that suspended owns the decision; the hooks behind it do not
 *    run.  Keeping an iterator alive across a suspension would mean holding
 *    pointers into a chain that a module unload may rewrite in between.
 *
 *  - The hold has a deadline, and expiring counts as #HOOK_DENY.  A module
 *    that suspends a veto point is deciding whether something may happen;
 *    letting it through because the module never answered would be exactly
 *    the outcome it was asked to prevent.  Unloading the module while it
 *    owes an answer ends the same way.
 *
 * All of this runs on the main thread.  A worker thread must never call
 * hook_resume(): it hands its result back through the worker's completion
 * callback, which the main thread runs, and that is where the resume goes.
 */

/** Called by the server when a suspended operation gets its answer.
 *
 * @param[in] data Opaque pointer the call site passed to
 *   hook_run_suspendable().
 * @param[in] result #HOOK_DENY to refuse; anything else to proceed.
 * @param[in] reason Why it was refused, or NULL.
 */
typedef void (*HookResumeFn)(void* data, enum HookResult result,
                             const char* reason);

/** Run a hook chain that a module may hold.
 *
 * Identical to hook_run() except that a #HOOK_PENDING is honoured: the
 * server records what it needs to come back, and the caller must return
 * without applying the operation.
 *
 * @param[in] type Hook point to run.
 * @param[in,out] ctx Context; HookContext::hc_token is set by this call.
 * @param[in] client Client the operation is about, for cancellation when it
 *   exits.  May be NULL.
 * @param[in] done Called with \a data when the answer arrives, when the
 *   deadline passes, or when the module that suspended goes away.  Never
 *   called before this function returns.
 * @param[in] data Opaque pointer for \a done.
 * @param[in] deadline Absolute time (CurrentTime + n) after which the hold
 *   is refused.  Must be in the future.
 * @return #HOOK_PENDING if the operation was suspended, otherwise what
 *   hook_run() would have returned.
 */
extern enum HookResult hook_run_suspendable(enum HookType type,
                                            struct HookContext* ctx,
                                            struct Client* client,
                                            HookResumeFn done, void* data,
                                            time_t deadline);

/** Answer a suspended hook.
 *
 * Called by the module that returned #HOOK_PENDING, on the main thread,
 * exactly once per token.  A token that is not outstanding -- because the
 * client left, the deadline passed or the answer was already given -- is
 * not an error: the module is told so and the call does nothing.
 *
 * @param[in] token The value read from HookContext::hc_token.
 * @param[in] result #HOOK_DENY to refuse the operation, #HOOK_ALLOW or
 *   #HOOK_CONTINUE to let it proceed.
 * @param[in] reason Text to explain a refusal, or NULL.
 * @return Non-zero if the token was outstanding and the operation resumed.
 */
extern int hook_resume(hook_token_t token, enum HookResult result,
                       const char* reason);

/** Drop every hold on a client, without resuming anything.
 *
 * Called when the client is leaving: the operation it was waiting for no
 * longer has anybody to apply it to, and the completion callback must not
 * run on a client that is being freed.
 */
extern void hook_pending_cancel(struct Client* client);

/** Refuse every hold whose deadline has passed.
 * @param[in] now Current time.
 * @return Number of holds that expired.
 */
extern int hook_pending_expire(time_t now);

/** Earliest deadline of any outstanding hold, or 0 if there are none. */
extern time_t hook_pending_deadline(void);

/** Number of operations currently held by a module. */
extern unsigned int hook_pending_count(void);

extern void hooks_init(void);

#endif /* INCLUDED_hooks_h */
