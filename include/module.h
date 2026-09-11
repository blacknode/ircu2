#ifndef INCLUDED_module_h
#define INCLUDED_module_h
/*
 * IRC - Internet Relay Chat, include/module.h
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
 * @brief Public API for loadable modules.
 *
 * A module is a shared object that the server loads with dlopen() at run
 * time.  It exports exactly one symbol, #ircu_module, through which the
 * server reaches everything else.
 *
 * Modules run inside the server process and share its address space.  A
 * module that misbehaves takes the server with it; see doc/readme.modules.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif
#ifndef INCLUDED_ircd_handler_h
#include "ircd_handler.h"
#endif
#ifndef INCLUDED_hooks_h
#include "hooks.h"
#endif
#ifndef INCLUDED_client_h
#include "client.h"     /* flag_t, HasUFlag() */
#endif
#ifndef INCLUDED_channel_h
#include "channel.h"    /* chanmode_t, HasCFlag() */
#endif
#ifndef INCLUDED_worker_h
#include "worker.h"     /* struct WorkTask, WorkerMainFn */
#endif

struct Client;
struct ModuleHandle;

/** ABI version of the module interface.
 *
 * The server refuses to load a module built against a different value.
 * There is no backwards compatibility: when this changes, modules are
 * recompiled.  A mismatched pointer layout in a shared address space is
 * not a failure worth being lenient about.
 */
#define IRCU_MODULE_ABI 4

/** Description of a module, exported by the shared object.
 *
 * Every module must define exactly one object of this type, named
 * @c ircu_module and with external linkage.
 */
struct ModuleInfo {
  unsigned int mi_abi;            /**< #IRCU_MODULE_ABI at compile time. */
  const char*  mi_name;           /**< Short name, e.g. "nocaps". */
  const char*  mi_version;        /**< Module version string. */
  const char*  mi_author;         /**< Author of the module. */
  const char*  mi_description;    /**< One-line description. */

  /** Called once after the module is loaded.
   * @param[in] mod Handle to use when registering commands and hooks.
   * @return Zero to keep the module loaded, non-zero to abort the load.
   */
  int  (*mi_init)(struct ModuleHandle* mod);

  /** Called once before the module is unloaded.
   *
   * Anything registered through the module API is reverted by the server
   * whether or not this does it; the callback is for the module's own
   * resources.
   * @param[in] mod Handle for this module.
   */
  void (*mi_fini)(struct ModuleHandle* mod);

  /** Called on /REHASH when the module stays loaded.  May be NULL.
   * @param[in] mod Handle for this module.
   */
  void (*mi_rehash)(struct ModuleHandle* mod);
};

/*
 * Accessors for a loaded module.  Modules use these rather than reaching
 * into struct ModuleHandle, which is private to the server.
 */
extern const char* module_name(const struct ModuleHandle* mod);
extern const char* module_path(const struct ModuleHandle* mod);
extern const char* module_file(const struct ModuleHandle* mod);
/** Nick that loaded the module, or NULL if the configuration file did. */
extern const char* module_loaded_by(const struct ModuleHandle* mod);
extern const char* module_version(const struct ModuleHandle* mod);
extern const char* module_description(const struct ModuleHandle* mod);

/*
 * Registering commands.
 *
 * Everything a module registers is tracked against its handle and reverted
 * when the module is unloaded, whether or not mi_fini remembers to do it.
 */

/** Register a command.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] cmd Command name, e.g. "SPAMFILTER".
 * @param[in] tok P10 token, or NULL to use the command name.
 * @param[in] parameters Maximum number of parameters to split the line
 *   into, NOT a minimum: everything past this many arrives in the last
 *   one.  Pass MAXPARA unless the command takes free-form trailing text.
 *   Handlers check their own minimum with need_more_params().
 * @param[in] flags Bitwise combination of MFLG_* values.
 * @param[in] handlers One handler per HandlerType; NULL entries become
 *   m_ignore.
 * @return Non-zero on success, zero if the name or token is already taken.
 */
extern int module_add_command(struct ModuleHandle* mod, const char* cmd,
                              const char* tok, unsigned int parameters,
                              unsigned int flags,
                              MessageHandler handlers[]);

/** Remove a command this module registered.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] cmd Command name to remove.
 * @return Non-zero if the command was found and removed.
 */
extern int module_del_command(struct ModuleHandle* mod, const char* cmd);

/** Number of commands a module currently has registered. */
extern unsigned int module_command_count(const struct ModuleHandle* mod);

/*
 * Registering user modes.
 *
 * A module asks for a mode letter and gets a bit back; it does not choose
 * the bit, because two modules that both chose the same one would share a
 * flag without either author noticing.  Test the bit on a client with
 * HasUFlag(), set it with SetUFlag(), clear it with ClrUFlag().
 *
 * Like commands and hooks, a mode is reverted when the module unloads:
 * every user still carrying it is stripped of it, and the change is
 * announced as an ordinary "-<mode>" so neither they nor the rest of the
 * network are left believing it is still set.
 */

/** Register a user mode.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] mode Mode letter, A-Z or a-z.
 * @param[out] flag Receives the bit the server assigned, or zero on
 *   failure.  May be NULL, though a module that never tests its own mode
 *   has little use for it.
 * @return Non-zero on success; zero if the letter is not a letter, if it
 *   is already taken by the core or by another module, or if the server
 *   has no free bit left.
 */
extern int module_add_user_mode(struct ModuleHandle* mod, char mode,
                                flag_t* flag);

/** Remove a user mode this module registered.
 *
 * Every user that has the mode set loses it, the same way an unload would
 * do it.  A module cannot remove a core mode, nor one another module
 * registered.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] mode Mode letter to remove.
 * @return Non-zero if the mode was found and removed.
 */
extern int module_del_user_mode(struct ModuleHandle* mod, char mode);

/** Number of user modes a module currently has registered. */
extern unsigned int module_user_mode_count(const struct ModuleHandle* mod);

/*
 * Registering channel modes.
 *
 * The same shape as the user modes, with one difference that matters: the
 * bit is not handed out, it follows from the letter ('A'-'Z' take bits
 * 0-25, 'a'-'z' bits 26-51).  So the mode a module registers is the same
 * bit on every server that loads the module, and two servers built from
 * the same sources never have to agree on anything at run time.  Test the
 * bit on a channel with HasCFlag(), set it with SetCFlag(), clear it with
 * ClrCFlag().
 *
 * The list of registered modes is readable through channel_chan_modes(),
 * as a pointer to const: it is the server's list, and a module reads it
 * rather than reaching into it.
 *
 * Like commands and hooks, a mode is reverted when the module unloads:
 * every channel still carrying it is stripped of it, and the change is
 * announced as an ordinary "-<mode>" to the members and to the network,
 * so that no channel is left believing it enforces a policy that nothing
 * implements any more.
 */

/** Register a channel mode.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] mode Mode letter, A-Z or a-z.
 * @param[out] flag Receives the bit the letter maps to, or zero on
 *   failure.  May be NULL, though a module that never tests its own mode
 *   has little use for it.
 * @return Non-zero on success; zero if the letter is not a letter, or if
 *   it is already taken by the core or by another module.
 */
extern int module_add_chan_mode(struct ModuleHandle* mod, char mode,
                                chanmode_t* flag);

/** Remove a channel mode this module registered.
 *
 * Every channel that has the mode set loses it, the same way an unload
 * would do it.  A module cannot remove a core mode, nor one another
 * module registered.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] mode Mode letter to remove.
 * @return Non-zero if the mode was found and removed.
 */
extern int module_del_chan_mode(struct ModuleHandle* mod, char mode);

/** Number of channel modes a module currently has registered. */
extern unsigned int module_chan_mode_count(const struct ModuleHandle* mod);

/*
 * Handing work to another thread.
 *
 * A hook or a command handler runs in the main thread, in the middle of
 * serving a client, so a module that needs to wait for something -- a
 * database, an HTTP API, a password hash worth the name -- cannot simply do
 * it there without stalling every other client.  It submits the work
 * instead, and gets called back in the main thread when it is done.
 *
 * The rules for what a module may touch from a worker thread are in
 * worker.h and doc/readme.workers.  They are short and they are absolute:
 * a worker thread that touches core state corrupts it, silently, under
 * load, in a way no test will reproduce.
 *
 * Both calls need FEAT_WORKER_THREADS to be non-zero and both fail
 * gracefully when it is not, which is the default.  A module that needs
 * workers checks the return value and says so, rather than assuming.
 *
 * Work is tracked against the module handle, like everything else here.
 * Unloading the module cancels whatever it has queued, waits for whatever
 * is running -- there is no safe alternative to waiting, since the code in
 * the worker is about to be unmapped -- and stops its dedicated workers.
 */

/** Submit a task to the worker pool.
 *
 * @param[in] mod Handle passed to mi_init.
 * @param[in] task Task from worker_task_new(), with wt_work set.  On
 *   success the server owns it and frees it after wt_done has run.
 * @return Non-zero on success.  Zero if the pool is off or its queue is
 *   full, in which case the task is still the caller's to free.
 */
extern int module_submit_work(struct ModuleHandle* mod,
                              struct WorkTask* task);

/** Start a thread with a loop of its own.
 *
 * For work that is not a series of short tasks: a listening socket, a
 * subscription, anything that has to stay up.  The thread hands results
 * back with worker_post().
 *
 * Safe to call from mi_init even though the worker subsystem is not up yet
 * at that point: the request is held and the thread starts once the
 * configuration file has been read.
 *
 * @param[in] mod Handle passed to mi_init.
 * @param[in] name Short name, for logs and /STATS M.
 * @param[in] fn The thread body.
 * @param[in] arg Passed through to \a fn.
 * @return The worker, or NULL.  The server owns it; it is stopped when the
 *   module unloads (before mi_fini runs), when WORKER_THREADS is set to
 *   zero, or earlier with module_stop_worker().
 */
extern struct Worker* module_spawn_worker(struct ModuleHandle* mod,
                                          const char* name, WorkerMainFn fn,
                                          void* arg);

/** Stop a dedicated worker this module started.
 *
 * Waits for the thread to return.  Not required: unloading the module does
 * the same thing, and does it before mi_fini is called, so a module that
 * stops its worker from mi_fini is stopping one the server already
 * stopped.  That is fine -- the handle is checked before it is read, and
 * a worker that is already gone makes this return zero and do nothing --
 * but it is nothing more than tidiness.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] worker Worker to stop, or NULL.  Invalid once this returns.
 * @return Non-zero if it was this module's worker and it was stopped;
 *   zero if it was not, or if it had already been stopped.
 */
extern int module_stop_worker(struct ModuleHandle* mod,
                              struct Worker* worker);

/** Tasks this module has submitted and not yet had delivered. */
extern unsigned int module_work_count(const struct ModuleHandle* mod);

/** Dedicated workers this module currently has running. */
extern unsigned int module_worker_count(const struct ModuleHandle* mod);

/*
 * Registering hooks.  See hooks.h for the hook points and what each one
 * may do.  Like commands, hooks are reverted when the module unloads.
 */

/** Attach a callback to a lifecycle hook.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] type Hook point, from enum HookType.
 * @param[in] fn Callback to run.
 * @param[in] priority Lower numbers run earlier; HOOK_PRIORITY_DEFAULT if
 *   the module does not care.
 * @param[in] user Opaque pointer handed back to the callback.
 * @return Non-zero on success.
 */
extern int module_add_hook(struct ModuleHandle* mod, enum HookType type,
                           HookFn fn, int priority, void* user);

/** Detach a callback this module attached.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] type Hook point it was attached to.
 * @param[in] fn The callback to detach.
 * @return Non-zero if it was found and detached.
 */
extern int module_del_hook(struct ModuleHandle* mod, enum HookType type,
                           HookFn fn);

/*
 * Server-side interface.  Not for use by modules.
 */
struct StatDesc;
extern void module_stats(struct Client* sptr, const struct StatDesc* sd,
                         char* param);

extern void module_init(void);
/** Unload every module, leaving the module system up. */
extern void module_shutdown(void);
/** Shut down and release the module system; main() only, once, at exit. */
extern void module_close(void);

/** Load MOD_PATH/<name>.so; \a name carries no directory and no suffix.
 * \a loaded_by is the loading operator's nick, or NULL for the config file.
 */
extern struct ModuleHandle* module_load(const char* name,
                                        const char* loaded_by,
                                        const char** errstr);
extern int module_unload(struct ModuleHandle* mod);
/** Find a module by the name it declares in its #ModuleInfo. */
extern struct ModuleHandle* module_find(const char* name);
/** Find a module by the name it was loaded by. */
extern struct ModuleHandle* module_find_file(const char* name);

/** Iterate over loaded modules; pass NULL to start. */
extern struct ModuleHandle* module_next(struct ModuleHandle* mod);

extern unsigned int module_count(void);

/** Non-zero while a module callback is on the stack.
 *
 * Unloading a module from inside one of its own callbacks would free the
 * code that is currently executing, so the server refuses to do it.
 */
extern int module_in_callback(void);

/** Record that the configuration mentioned this module, for rehash. */
extern void module_mark(struct ModuleHandle* mod);
extern void module_unmark_all(void);
extern void module_sweep(void);
extern int  module_changed_on_disk(const struct ModuleHandle* mod);
extern void module_rehash_notify(struct ModuleHandle* mod);

#endif /* INCLUDED_module_h */
