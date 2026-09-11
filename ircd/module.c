/*
 * IRC - Internet Relay Chat, ircd/module.c
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
 * @brief Loading and unloading of shared-object modules.
 */
#include "config.h"

#include "channel.h"
#include "client.h"
#include "hooks.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "parse.h"
#include "s_debug.h"
#include "send.h"
#include "worker.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/** Directory the loader resolves module names against.
 *
 * MOD_PATH comes from config.h, i.e. from the IRCU_MPATH build setting.  The
 * indirection exists so the unit test can point the loader at its fixtures
 * without a second copy of the resolution logic.
 */
#ifndef IRCU_MODULE_DIR
#define IRCU_MODULE_DIR MOD_PATH
#endif

/** A command registered by a module. */
struct ModuleCommand {
  struct ModuleCommand *mc_next; /**< Next command from the same module. */
  struct Message *mc_msg;        /**< Message installed in the tries. */
};

/** A user mode registered by a module. */
struct ModuleUserMode {
  struct ModuleUserMode *mu_next; /**< Next mode from the same module. */
  char mu_char;                   /**< Mode letter. */
  flag_t mu_flag;                 /**< Bit the server assigned to it. */
};

/** A channel mode registered by a module. */
struct ModuleChanMode {
  struct ModuleChanMode *mc_next; /**< Next mode from the same module. */
  char mc_char;                   /**< Mode letter. */
  chanmode_t mc_flag;             /**< Bit the letter maps to. */
};

/** A module the server has loaded. */
struct ModuleHandle {
  struct ModuleHandle *mh_next;  /**< Next module in #ModuleManager->mod_list. */
  void *mh_dl;                   /**< Handle returned by dlopen(). */
  struct ModuleInfo *mh_info;    /**< The module's exported description. */
  char *mh_file;                 /**< Name the module was loaded by. */
  char *mh_path;                 /**< Path the module was loaded from. */
  char *mh_relpath;              /**< The same, relative to #MOD_PATH. */
  char *mh_dir;                  /**< Directory of #mh_path. */
  time_t mh_mtime;               /**< Modification time when loaded. */
  int mh_marked;                 /**< Seen in the running configuration. */
  struct ModuleCommand *mh_cmds; /**< Commands this module registered. */
  struct ModuleUserMode *mh_umodes; /**< User modes this module registered. */
  struct ModuleChanMode *mh_cmodes; /**< Channel modes it registered. */
  char *mh_loaded_by;            /**< Nick that loaded it, or NULL for the
                                      configuration file.  A copy: the client
                                      may be long gone by the time anyone
                                      asks. */
};

/* A module manager */
struct ModuleManager {
  unsigned int mod_count;
  unsigned int mod_cb_depth;
  struct ModuleHandle *mod_list;
};

/** Manager (stats & more) */
struct ModuleManager *manager;

/** Where the last failure was explained; see module_load().  Big enough
 * that the ambiguity message, which quotes two relative paths, never
 * truncates.
 */
static char errbuf[1024];

static int module_unload_internal(struct ModuleHandle *mod, int quiet);

/** Get the name of a module.
 * @param[in] mod Module to query.
 * @return The module's short name.
 */
const char *module_name(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return mod->mh_info->mi_name;
}

/** Get the path a module was loaded from.
 *
 * This is an absolute path, for the module's own use; what an operator is
 * shown is module_relpath().
 *
 * @param[in] mod Module to query.
 * @return Filesystem path of the shared object.
 */
const char *module_path(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return mod->mh_path;
}

/** Get where a module sits under the module directory.
 *
 * "<type>/<name>.so" or "<type>/<name>/<name>.so": enough to find the
 * module in the tree, and nothing about where that tree is on the host,
 * which is why the listings print this and not module_path().
 *
 * @param[in] mod Module to query.
 * @return Path of the shared object relative to #MOD_PATH.
 */
const char *module_relpath(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return mod->mh_relpath;
}

/** Get the directory a module's shared object is in.
 *
 * A module built from a directory has its resources copied here, beside
 * the shared object, so this is how it finds them.
 *
 * @param[in] mod Module to query.
 * @return Absolute directory, without a trailing slash.
 */
const char *module_dir(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return mod->mh_dir;
}

/** Get the name a module was loaded by.
 *
 * This is the name that appears in a Module{} block or in /MODULE LOAD, and
 * the name to hand back to module_load() to load it again.  It names the
 * file under #MOD_PATH, and need not match the name the module declares in
 * its #ModuleInfo.
 *
 * @param[in] mod Module to query.
 * @return File name, without the directory or the ".so" suffix.
 */
const char *module_file(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return mod->mh_file;
}

/** Get the nick of the operator that loaded a module.
 *
 * The nick is copied at load time, so it survives the client disconnecting
 * (and does not follow a later nick change).
 *
 * @param[in] mod Module to query.
 * @return Nick, or NULL when the module came from the configuration file.
 */
const char *module_loaded_by(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return mod->mh_loaded_by;
}

/** Get the version string a module declares.
 * @param[in] mod Module to query.
 * @return Version string, or "?" if the module declares none.
 */
const char *module_version(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return mod->mh_info->mi_version ? mod->mh_info->mi_version : "?";
}

/** Get the description a module declares.
 * @param[in] mod Module to query.
 * @return Description, or "" if the module declares none.
 */
const char *module_description(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return mod->mh_info->mi_description ? mod->mh_info->mi_description : "";
}

/** Return the number of currently loaded modules. */
unsigned int module_count(void) {
  if (!manager)
    return 0;
  return manager->mod_count;
}

/** Report whether a module callback is currently executing. */
int module_in_callback(void) {
  if (!manager)
    return 0;
  return manager->mod_cb_depth > 0;
}

/** Iterate over the loaded modules.
 * @param[in] mod Previously returned module, or NULL to start.
 * @return Next module, or NULL when the list is exhausted.
 */
struct ModuleHandle *module_next(struct ModuleHandle *mod) {
  if (mod)
    return mod->mh_next;

  return manager ? manager->mod_list : 0;
}

/** Find a loaded module by name.
 * @param[in] name Module name to look for; compared case-insensitively.
 * @return Matching module, or NULL.
 */
struct ModuleHandle *module_find(const char *name) {
  struct ModuleHandle *mod;

  if (!manager || !name)
    return 0;

  for (mod = manager->mod_list; mod; mod = mod->mh_next)
    if (0 == ircd_strcmp(mod->mh_info->mi_name, name))
      return mod;

  return 0;
}

/** Find a loaded module by the name it was loaded by.
 *
 * The name is a file name under #MOD_PATH, so it is compared exactly: the
 * filesystem this resolves against is case-sensitive.
 *
 * @param[in] name Name to look for.
 * @return Matching module, or NULL.
 */
struct ModuleHandle *module_find_file(const char *name) {
  struct ModuleHandle *mod;

  if (!manager || !name)
    return 0;

  for (mod = manager->mod_list; mod; mod = mod->mh_next)
    if (0 == strcmp(mod->mh_file, name))
      return mod;

  return 0;
}

/** Get the modification time of a file.
 * @param[in] path File to stat.
 * @return Modification time, or zero if the file cannot be stat()ed.
 */
static time_t module_mtime(const char *path) {
  struct stat sb;

  if (stat(path, &sb) < 0)
    return 0;

  return sb.st_mtime;
}

/** Test whether a module's file changed since it was loaded.
 * @param[in] mod Module to check.
 * @return Non-zero if the file on disk is newer or has gone missing.
 */
int module_changed_on_disk(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return module_mtime(mod->mh_path) != mod->mh_mtime;
}

/** Register a command on behalf of a module.
 * @param[in] mod Module registering the command.
 * @param[in] cmd Command name.
 * @param[in] tok P10 token, or NULL to use the command name.
 * @param[in] parameters Maximum number of parameters to split into.
 * @param[in] flags Bitwise combination of MFLG_* values.
 * @param[in] handlers One handler per HandlerType.
 * @return Non-zero on success.
 */
int module_add_command(struct ModuleHandle *mod, const char *cmd,
                       const char *tok, unsigned int parameters,
                       unsigned int flags, MessageHandler handlers[]) {
  struct ModuleCommand *mc;
  struct Message *msg;

  assert(0 != mod);
  assert(0 != cmd);

  msg = parse_add_command(cmd, tok, parameters, flags, handlers);
  if (!msg) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Module %s could not register command %s: name already in use",
              mod->mh_info->mi_name, cmd);
    return 0;
  }

  mc = (struct ModuleCommand *)MyCalloc(1, sizeof(struct ModuleCommand));
  mc->mc_msg = msg;
  mc->mc_next = mod->mh_cmds;
  mod->mh_cmds = mc;

  return 1;
}

/** Remove a command a module registered.
 * @param[in] mod Module that owns the command.
 * @param[in] cmd Command name to remove.
 * @return Non-zero if the command was found and removed.
 */
int module_del_command(struct ModuleHandle *mod, const char *cmd) {
  struct ModuleCommand **mc_p;
  struct ModuleCommand *mc;

  assert(0 != mod);
  assert(0 != cmd);

  for (mc_p = &mod->mh_cmds; (mc = *mc_p); mc_p = &mc->mc_next) {
    if (0 == ircd_strcmp(mc->mc_msg->cmd, cmd)) {
      *mc_p = mc->mc_next;
      parse_del_command(mc->mc_msg);
      MyFree(mc);
      return 1;
    }
  }

  return 0;
}

/** Return how many commands a module currently has registered. */
unsigned int module_command_count(const struct ModuleHandle *mod) {
  struct ModuleCommand *mc;
  unsigned int count = 0;

  assert(0 != mod);

  for (mc = mod->mh_cmds; mc; mc = mc->mc_next)
    count++;

  return count;
}

/** Remove every command a module registered.
 * @param[in] mod Module being torn down.
 */
static void module_drop_commands(struct ModuleHandle *mod) {
  struct ModuleCommand *mc;
  struct ModuleCommand *next;

  for (mc = mod->mh_cmds; mc; mc = next) {
    next = mc->mc_next;
    parse_del_command(mc->mc_msg);
    MyFree(mc);
  }

  mod->mh_cmds = 0;
}

/** Register a user mode on behalf of a module.
 * @param[in] mod Module registering the mode.
 * @param[in] mode Mode letter.
 * @param[out] flag Receives the bit assigned, or zero on failure.
 * @return Non-zero on success.
 */
int module_add_user_mode(struct ModuleHandle *mod, char mode, flag_t *flag) {
  struct ModuleUserMode *mu;
  flag_t bit;
  int res;

  assert(0 != mod);

  if (flag)
    *flag = 0;

  /* Allocating is only a scan for a free bit; nothing is consumed until
   * client_append_user_mode() accepts the letter as well.
   */
  bit = client_alloc_user_mode_flag();
  if (!bit) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Module %s could not register user mode %c: no free mode bit",
              mod->mh_info->mi_name, mode);
    return 0;
  }

  res = client_append_user_mode(mode, bit);
  if (UMODE_APPEND_OK != res) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Module %s could not register user mode %c: %s",
              mod->mh_info->mi_name, mode,
              UMODE_INVALID_MODE == res ? "not a mode letter"
                                        : "already in use");
    return 0;
  }

  mu = (struct ModuleUserMode *)MyCalloc(1, sizeof(struct ModuleUserMode));
  mu->mu_char = mode;
  mu->mu_flag = bit;
  mu->mu_next = mod->mh_umodes;
  mod->mh_umodes = mu;

  if (flag)
    *flag = bit;

  return 1;
}

/** Remove a user mode a module registered.
 * @param[in] mod Module that owns the mode.
 * @param[in] mode Mode letter to remove.
 * @return Non-zero if the mode was found and removed.
 */
int module_del_user_mode(struct ModuleHandle *mod, char mode) {
  struct ModuleUserMode **mu_p;
  struct ModuleUserMode *mu;

  assert(0 != mod);

  /* Only this module's own list is searched: a module may not take a mode
   * away from the core or from another module.
   */
  for (mu_p = &mod->mh_umodes; (mu = *mu_p); mu_p = &mu->mu_next) {
    if (mu->mu_char == mode) {
      *mu_p = mu->mu_next;
      client_remove_user_mode(mu->mu_char);
      MyFree(mu);
      return 1;
    }
  }

  return 0;
}

/** Return how many user modes a module currently has registered. */
unsigned int module_user_mode_count(const struct ModuleHandle *mod) {
  struct ModuleUserMode *mu;
  unsigned int count = 0;

  assert(0 != mod);

  for (mu = mod->mh_umodes; mu; mu = mu->mu_next)
    count++;

  return count;
}

/** Render a module's mode letters as "(+xy)" for /STATS M.
 * @param[in] mod Module to describe.
 * @return Pointer to a static buffer, empty if the module has no modes.
 */
static const char *module_user_mode_chars(const struct ModuleHandle *mod) {
  static char buf[USERMODE_CHARS_LEN + 4];
  struct ModuleUserMode *mu;
  size_t len = 0;

  if (!mod->mh_umodes)
    return "";

  buf[len++] = '(';
  buf[len++] = '+';
  for (mu = mod->mh_umodes; mu && len + 2 < sizeof(buf); mu = mu->mu_next)
    buf[len++] = mu->mu_char;
  buf[len++] = ')';
  buf[len] = '\0';

  return buf;
}

/** Remove every user mode a module registered.
 *
 * client_remove_user_mode() strips the mode from every user that still has
 * it and announces the change, so no one is left believing a mode nothing
 * implements any more is still in force.
 *
 * @param[in] mod Module being torn down.
 */
static void module_drop_user_modes(struct ModuleHandle *mod) {
  struct ModuleUserMode *mu;
  struct ModuleUserMode *next;

  for (mu = mod->mh_umodes; mu; mu = next) {
    next = mu->mu_next;
    client_remove_user_mode(mu->mu_char);
    MyFree(mu);
  }

  mod->mh_umodes = 0;
}

/** Register a channel mode on behalf of a module.
 * @param[in] mod Module registering the mode.
 * @param[in] mode Mode letter.
 * @param[out] flag Receives the bit the letter maps to, or zero on
 *   failure.
 * @return Non-zero on success.
 */
int module_add_chan_mode(struct ModuleHandle *mod, char mode,
                         chanmode_t *flag) {
  struct ModuleChanMode *mc;
  chanmode_t bit;
  int res;

  assert(0 != mod);

  if (flag)
    *flag = 0;

  /* The bit is not handed out, it follows from the letter; asking for it
   * consumes nothing, and a letter that has no bit is not a letter.
   */
  bit = channel_chan_mode_flag(mode);

  res = channel_append_chan_mode(mode, bit);
  if (CMODE_APPEND_OK != res) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Module %s could not register channel mode %c: %s",
              mod->mh_info->mi_name, mode,
              CMODE_INVALID_MODE == res ? "not a mode letter"
                                        : "already in use");
    return 0;
  }

  mc = (struct ModuleChanMode *)MyCalloc(1, sizeof(struct ModuleChanMode));
  mc->mc_char = mode;
  mc->mc_flag = bit;
  mc->mc_next = mod->mh_cmodes;
  mod->mh_cmodes = mc;

  if (flag)
    *flag = bit;

  return 1;
}

/** Remove a channel mode a module registered.
 * @param[in] mod Module that owns the mode.
 * @param[in] mode Mode letter to remove.
 * @return Non-zero if the mode was found and removed.
 */
int module_del_chan_mode(struct ModuleHandle *mod, char mode) {
  struct ModuleChanMode **mc_p;
  struct ModuleChanMode *mc;

  assert(0 != mod);

  /* Only this module's own list is searched: a module may not take a mode
   * away from the core or from another module.
   */
  for (mc_p = &mod->mh_cmodes; (mc = *mc_p); mc_p = &mc->mc_next) {
    if (mc->mc_char == mode) {
      *mc_p = mc->mc_next;
      channel_remove_chan_mode(mc->mc_char);
      MyFree(mc);
      return 1;
    }
  }

  return 0;
}

/** Return how many channel modes a module currently has registered. */
unsigned int module_chan_mode_count(const struct ModuleHandle *mod) {
  struct ModuleChanMode *mc;
  unsigned int count = 0;

  assert(0 != mod);

  for (mc = mod->mh_cmodes; mc; mc = mc->mc_next)
    count++;

  return count;
}

/** Render a module's channel mode letters as "(+xy)" for /STATS M.
 * @param[in] mod Module to describe.
 * @return Pointer to a static buffer, empty if the module has no modes.
 */
static const char *module_chan_mode_chars(const struct ModuleHandle *mod) {
  static char buf[CHANMODE_CHARS_LEN + 4];
  struct ModuleChanMode *mc;
  size_t len = 0;

  if (!mod->mh_cmodes)
    return "";

  buf[len++] = '(';
  buf[len++] = '+';
  for (mc = mod->mh_cmodes; mc && len + 2 < sizeof(buf); mc = mc->mc_next)
    buf[len++] = mc->mc_char;
  buf[len++] = ')';
  buf[len] = '\0';

  return buf;
}

/** Remove every channel mode a module registered.
 *
 * channel_remove_chan_mode() strips the mode from every channel that
 * still has it and announces the change, to the members and to the rest
 * of the network: a channel is the network's, and leaving a policy
 * standing that nothing implements any more would be worse than losing
 * it everywhere.
 *
 * @param[in] mod Module being torn down.
 */
static void module_drop_chan_modes(struct ModuleHandle *mod) {
  struct ModuleChanMode *mc;
  struct ModuleChanMode *next;

  for (mc = mod->mh_cmodes; mc; mc = next) {
    next = mc->mc_next;
    channel_remove_chan_mode(mc->mc_char);
    MyFree(mc);
  }

  mod->mh_cmodes = 0;
}

/** Submit a task to the worker pool on a module's behalf.
 * @param[in] mod Module submitting the work.
 * @param[in] task Task to run.
 * @return Non-zero on success.
 */
int module_submit_work(struct ModuleHandle *mod, struct WorkTask *task) {
  assert(0 != mod);
  assert(0 != task);

  return worker_submit_owned(mod, task);
}

/** Start a dedicated worker on a module's behalf.
 * @param[in] mod Module starting the thread.
 * @param[in] name Short name for logs and /STATS M.
 * @param[in] fn The thread body.
 * @param[in] arg Passed through to \a fn.
 * @return The worker, or NULL.
 */
struct Worker *module_spawn_worker(struct ModuleHandle *mod, const char *name,
                                   WorkerMainFn fn, void *arg) {
  assert(0 != mod);
  assert(0 != fn);

  return worker_spawn_owned(mod, name, fn, arg);
}

/** Stop a dedicated worker a module started.
 *
 * The ownership check is the point: a module must not be able to stop
 * another module's thread, whether by accident or otherwise.  It is made
 * in worker.c and not here, because the handle may already be stale --
 * the server stops a module's workers before mi_fini runs, and a module
 * that stops its own thread from mi_fini is handing back a pointer to
 * freed memory -- and only worker.c can tell without reading it.
 *
 * @param[in] mod Module that owns the worker.
 * @param[in] worker Worker to stop.
 * @return Non-zero if it was stopped; zero if it was not this module's,
 *   or was already gone.
 */
int module_stop_worker(struct ModuleHandle *mod, struct Worker *worker) {
  assert(0 != mod);

  return worker_stop_owned(mod, worker);
}

/** Tasks a module has submitted and not yet had delivered.
 * @param[in] mod Module to query.
 */
unsigned int module_work_count(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return worker_module_tasks(mod);
}

/** Dedicated workers a module currently has running.
 * @param[in] mod Module to query.
 */
unsigned int module_worker_count(const struct ModuleHandle *mod) {
  assert(0 != mod);
  return worker_module_workers(mod);
}

/** Attach a hook on behalf of a module.
 * @param[in] mod Module registering the hook.
 * @param[in] type Hook point.
 * @param[in] fn Callback.
 * @param[in] priority Lower runs earlier.
 * @param[in] user Opaque pointer for the callback.
 * @return Non-zero on success.
 */
int module_add_hook(struct ModuleHandle *mod, enum HookType type, HookFn fn,
                    int priority, void *user) {
  assert(0 != mod);
  return hook_add(mod, mod->mh_info->mi_name, type, fn, priority, user);
}

/** Detach a hook a module attached.
 * @param[in] mod Module that owns the hook.
 * @param[in] type Hook point.
 * @param[in] fn Callback to detach.
 * @return Non-zero if it was found.
 */
int module_del_hook(struct ModuleHandle *mod, enum HookType type, HookFn fn) {
  assert(0 != mod);
  return hook_del(mod, type, fn);
}

/** Turn a module name into the path of its shared object.
 *
 * The name is a bare name: it may not contain a directory separator and
 * may not be a relative directory reference, so a Module{} block or a
 * /MODULE LOAD cannot reach outside the module directory the server was
 * built with.
 *
 * The module directory is organised by type, and a name says nothing
 * about the type, so every type directory is searched for the two shapes
 * a module comes in: <type>/<name>.so, a module built from one source,
 * and <type>/<name>/<name>.so, one built from a directory.  Exactly one
 * match is a module; none is an unknown name, and two is refused rather
 * than picked from, because whichever won would be an accident of readdir
 * order.  Nothing directly in the module directory is considered.
 *
 * @param[in] name Module name, without directory or ".so" suffix.
 * @param[out] path Receives the absolute path of the shared object.
 * @param[in] pathlen Size of \a path.
 * @param[out] relpath Receives the same path relative to #MOD_PATH.
 * @param[in] rellen Size of \a relpath.
 * @param[out] errstr If non-NULL, receives the reason the name was refused.
 * @return Non-zero on success.
 */
static int module_resolve(const char *name, char *path, size_t pathlen,
                          char *relpath, size_t rellen, const char **errstr) {
  struct dirent *ent;
  DIR *dir;
  unsigned int found = 0;

  if (!name || !*name) {
    if (errstr)
      *errstr = "no module name given";
    return 0;
  }

  if (strchr(name, '/') || 0 == strcmp(name, ".") || 0 == strcmp(name, "..")) {
    if (errstr)
      *errstr = "a module is named by name, not by path";
    return 0;
  }

  if (!(dir = opendir(IRCU_MODULE_DIR))) {
    snprintf(errbuf, sizeof(errbuf), "cannot open the module directory: %s",
             strerror(errno));
    if (errstr)
      *errstr = errbuf;
    return 0;
  }

  while ((ent = readdir(dir))) {
    char candidate[1024];
    char rel[256];
    struct stat sb;
    int shape;

    /* Hidden entries are nobody's type directory, and this also covers
     * "." and "..".
     */
    if (ent->d_name[0] == '.')
      continue;

    if (snprintf(candidate, sizeof(candidate), "%s/%s", IRCU_MODULE_DIR,
                 ent->d_name) >= (int)sizeof(candidate)
        || stat(candidate, &sb) < 0 || !S_ISDIR(sb.st_mode))
      continue;

    for (shape = 0; shape < 2; shape++) {
      int written;

      if (shape == 0)
        written = snprintf(rel, sizeof(rel), "%s/%s.so", ent->d_name, name);
      else
        written = snprintf(rel, sizeof(rel), "%s/%s/%s.so", ent->d_name, name,
                           name);
      if (written < 0 || (size_t)written >= sizeof(rel)
          || (size_t)written >= rellen)
        continue;

      written = snprintf(candidate, sizeof(candidate), "%s/%s",
                         IRCU_MODULE_DIR, rel);
      if (written < 0 || (size_t)written >= sizeof(candidate)
          || (size_t)written >= pathlen)
        continue;

      if (stat(candidate, &sb) < 0 || !S_ISREG(sb.st_mode))
        continue;

      if (found++) {
        snprintf(errbuf, sizeof(errbuf),
                 "module %s is ambiguous: found as modules/%s and modules/%s",
                 name, relpath, rel);
        closedir(dir);
        if (errstr)
          *errstr = errbuf;
        return 0;
      }

      strcpy(path, candidate);
      strcpy(relpath, rel);
    }
  }

  closedir(dir);

  if (!found) {
    snprintf(errbuf, sizeof(errbuf),
             "no module named %s: looked for <type>/%s.so and "
             "<type>/%s/%s.so under the module directory",
             name, name, name, name);
    if (errstr)
      *errstr = errbuf;
    return 0;
  }

  return 1;
}

/** Load a module by name from the module directory.
 *
 * The shared object is <name>.so under one of the type directories of
 * #MOD_PATH; see module_resolve().  On failure nothing is left behind: the
 * shared object is closed again and no handle is added to the module list.
 *
 * @param[in] name Module name, as a Module{} block or /MODULE LOAD gives it.
 * @param[in] loaded_by Nick of the operator loading it, or NULL when the
 *   configuration file is doing the loading.  It is copied, so the client
 *   it names may leave at any time afterwards.
 * @param[out] errstr If non-NULL, receives a human-readable reason on
 *   failure.  The string is owned by the caller only until the next call.
 * @return Handle for the loaded module, or NULL on failure.
 */
struct ModuleHandle *module_load(const char *name, const char *loaded_by,
                                 const char **errstr) {
  struct ModuleHandle *mod;
  struct ModuleInfo *info;
  char path[1024];
  char relpath[256];
  char *slash;
  void *dl;

  assert(0 != name);

  if (errstr)
    *errstr = 0;

  if (!manager) {
    if (errstr)
      *errstr = "the module system is not initialised";
    return 0;
  }

  if (!module_resolve(name, path, sizeof(path), relpath, sizeof(relpath),
                      errstr))
    return 0;

  if (module_find_file(name)) {
    if (errstr)
      *errstr = "module is already loaded";
    return 0;
  }

  /* RTLD_NOW so that unresolved symbols surface here rather than at some
   * arbitrary later moment inside a hook.  RTLD_LOCAL keeps one module's
   * symbols from satisfying another module's undefined references, which
   * would make load order significant.
   */
  dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!dl) {
    if (errstr) {
      ircd_strncpy(errbuf, dlerror(), sizeof(errbuf) - 1);
      errbuf[sizeof(errbuf) - 1] = '\0';
      *errstr = errbuf;
    }
    return 0;
  }

  info = (struct ModuleInfo *)dlsym(dl, "ircu_module");
  if (!info) {
    dlclose(dl);
    if (errstr)
      *errstr = "no ircu_module symbol; is this an ircu module?";
    return 0;
  }

  if (info->mi_abi != IRCU_MODULE_ABI) {
    snprintf(errbuf, sizeof(errbuf),
             "ABI mismatch: module was built for %u, server speaks %u",
             info->mi_abi, (unsigned int)IRCU_MODULE_ABI);
    dlclose(dl);
    if (errstr)
      *errstr = errbuf;
    return 0;
  }

  if (!info->mi_name || !*info->mi_name) {
    dlclose(dl);
    if (errstr)
      *errstr = "module declares no name";
    return 0;
  }

  if (module_find(info->mi_name)) {
    snprintf(errbuf, sizeof(errbuf), "a module named %s is already loaded",
             info->mi_name);
    dlclose(dl);
    if (errstr)
      *errstr = errbuf;
    return 0;
  }

  mod = (struct ModuleHandle *)MyCalloc(1, sizeof(struct ModuleHandle));
  mod->mh_dl = dl;
  mod->mh_info = info;
  DupString(mod->mh_file, name);
  if (loaded_by)
    DupString(mod->mh_loaded_by, loaded_by);
  DupString(mod->mh_path, path);
  DupString(mod->mh_relpath, relpath);
  /* module_resolve() built the path with at least one '/' in it. */
  DupString(mod->mh_dir, path);
  if ((slash = strrchr(mod->mh_dir, '/')))
    *slash = '\0';
  mod->mh_mtime = module_mtime(path);
  mod->mh_marked = 1;

  /* Link before mi_init so that anything the module registers can find its
   * own handle in the list.
   */
  mod->mh_next = manager->mod_list;
  manager->mod_list = mod;
  manager->mod_count++;

  if (info->mi_init) {
    int res;

    manager->mod_cb_depth++;
    res = (*info->mi_init)(mod);
    manager->mod_cb_depth--;

    if (res) {
      snprintf(errbuf, sizeof(errbuf),
               "module %s refused to initialise (returned %d)", info->mi_name,
               res);

      /* Unlink and tear down; mi_init is responsible for having left
       * nothing behind on its failure path, but the unload path below
       * reverts server-side registrations regardless.
       */
      module_unload_internal(mod, 1);

      if (errstr)
        *errstr = errbuf;
      return 0;
    }
  }

  log_write(LS_SYSTEM, L_INFO, 0, "Loaded module %s %s from %s", info->mi_name,
            info->mi_version ? info->mi_version : "?", path);

  return mod;
}

/** Unload a module and close its shared object.
 * @param[in] mod Module to unload.
 * @param[in] quiet Non-zero to skip the "Unloaded" log line, used when
 *   unwinding a load that never completed.
 * @return Non-zero on success, zero if the module cannot be unloaded now.
 */
static int module_unload_internal(struct ModuleHandle *mod, int quiet) {
  struct ModuleHandle **mod_p;
  char name[64];
  void *dl;

  assert(0 != mod);

  /* Unloading from inside a module callback would free the code that is
   * currently running.  Callers check this too, but assert here so a new
   * caller that forgets is caught in a debug build rather than in a
   * production crash dump.
   */
  if (module_in_callback()) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Refusing to unload module %s from inside a module callback",
              mod->mh_info->mi_name);
    return 0;
  }

  ircd_strncpy(name, mod->mh_info->mi_name, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';

  /* Before mi_fini, not after: a worker thread still executing this
   * module's code would be reading whatever mi_fini has just freed.  This
   * waits for work in flight, and blocks the server while it does -- there
   * is no correct alternative to waiting, because the code is about to be
   * unmapped.
   */
  worker_cancel_module(mod);

  if (mod->mh_info->mi_fini) {
    manager->mod_cb_depth++;
    (*mod->mh_info->mi_fini)(mod);
    manager->mod_cb_depth--;
  }

  /* Everything the module registered through the module API is reverted
   * here, after mi_fini has had its chance and before the code goes away.
   * A module that forgets to unregister its own commands still cannot
   * leave the trie pointing into an unmapped shared object.
   */
  module_drop_commands(mod);
  hook_del_module(mod);
  /* Modes last: taking a mode off a user or a channel announces a MODE
   * change, and the module's own hooks are already detached by then, so
   * none of its code runs on the way out.
   */
  module_drop_user_modes(mod);
  module_drop_chan_modes(mod);

  for (mod_p = &manager->mod_list; *mod_p; mod_p = &(*mod_p)->mh_next) {
    if (*mod_p == mod) {
      *mod_p = mod->mh_next;
      manager->mod_count--;
      break;
    }
  }

  dl = mod->mh_dl;
  MyFree(mod->mh_file);
  MyFree(mod->mh_loaded_by);
  MyFree(mod->mh_path);
  MyFree(mod->mh_relpath);
  MyFree(mod->mh_dir);
  MyFree(mod);

  /* dlclose() last: mh_info points into the object we are about to
   * unmap, so nothing may touch the handle after this.
   */
  dlclose(dl);

  if (!quiet)
    log_write(LS_SYSTEM, L_INFO, 0, "Unloaded module %s", name);

  return 1;
}

/** Unload a module and close its shared object.
 * @param[in] mod Module to unload.
 * @return Non-zero on success, zero if the module cannot be unloaded now.
 */
int module_unload(struct ModuleHandle *mod) {
  return module_unload_internal(mod, 0);
}

/** Mark a module as present in the running configuration. */
void module_mark(struct ModuleHandle *mod) {
  assert(0 != mod);
  mod->mh_marked = 1;
}

/** Clear the configuration mark on every loaded module.
 *
 * Called before re-reading the configuration; module_sweep() then unloads
 * whatever the new configuration did not mention.
 */
void module_unmark_all(void) {
  struct ModuleHandle *mod;

  if (!manager)
    return;

  for (mod = manager->mod_list; mod; mod = mod->mh_next)
    mod->mh_marked = 0;
}

/** Unload every module the running configuration no longer mentions. */
void module_sweep(void) {
  struct ModuleHandle *mod;
  struct ModuleHandle *next;

  if (!manager)
    return;

  for (mod = manager->mod_list; mod; mod = next) {
    next = mod->mh_next;
    if (!mod->mh_marked)
      module_unload(mod);
  }
}

/** Tell a module that the server rehashed, if it wants to know. */
void module_rehash_notify(struct ModuleHandle *mod) {
  assert(0 != mod);

  if (mod->mh_info->mi_rehash) {
    manager->mod_cb_depth++;
    (*mod->mh_info->mi_rehash)(mod);
    manager->mod_cb_depth--;
  }
}

/** Report loaded modules and hook activity for /STATS.
 * @param[in] sptr Client asking for the statistics.
 * @param[in] sd Stats descriptor (unused).
 * @param[in] param Extra parameter (unused).
 */
void module_stats(struct Client *sptr, const struct StatDesc *sd, char *param) {
  struct ModuleHandle *mod;
  int type;

  if (!manager)
    return;

  for (mod = manager->mod_list; mod; mod = mod->mh_next)
    send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
               ":Module %s %s: %u command%s, %u user mode%s%s%s, "
               "%u channel mode%s%s%s, from modules/%s, loaded by %s",
               mod->mh_info->mi_name, module_version(mod),
               module_command_count(mod),
               module_command_count(mod) == 1 ? "" : "s",
               module_user_mode_count(mod),
               module_user_mode_count(mod) == 1 ? "" : "s",
               module_user_mode_count(mod) ? " " : "",
               module_user_mode_chars(mod),
               module_chan_mode_count(mod),
               module_chan_mode_count(mod) == 1 ? "" : "s",
               module_chan_mode_count(mod) ? " " : "",
               module_chan_mode_chars(mod), mod->mh_relpath,
               mod->mh_loaded_by ? mod->mh_loaded_by : "the configuration");

  /* Only for modules that actually use workers: on a server where nothing
   * does, this section is silent.
   */
  for (mod = manager->mod_list; mod; mod = mod->mh_next) {
    unsigned int tasks = worker_module_tasks(mod);
    unsigned int workers = worker_module_workers(mod);

    if (tasks || workers)
      send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
                 ":Module %s: %u task%s in flight, %u dedicated worker%s",
                 mod->mh_info->mi_name, tasks, tasks == 1 ? "" : "s",
                 workers, workers == 1 ? "" : "s");
  }

  send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG, ":%u module%s loaded, ABI %u",
             manager->mod_count, manager->mod_count == 1 ? "" : "s",
             (unsigned int)IRCU_MODULE_ABI);

  /* Workers live next to modules in the operator's mental model, and this
   * is where an operator already looks; a stats letter of their own would
   * be one more thing to remember for four lines of output.
   */
  if (!worker_enabled())
    send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
               ":Workers disabled (WORKER_THREADS is 0)");
  else {
    send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
               ":Workers: %u pool thread%s, %u dedicated",
               worker_thread_count(),
               worker_thread_count() == 1 ? "" : "s",
               worker_dedicated_count());
    /* Split three ways rather than one "outstanding": a backlog that is all
     * queued means the pool is too small, and one that is all running means
     * something is taking far longer than it should.
     */
    send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
               ":Workers: %u queued, %u running, %u waiting to be delivered",
               worker_queued(), worker_running(), worker_undelivered());
    send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
               ":Workers: %u submitted, %u completed, %u rejected",
               worker_submitted(), worker_completed(), worker_rejected());
  }

  /* Only hook points that are in use or have fired: listing all eighteen
   * every time would bury the two lines an operator actually wants.
   */
  for (type = 0; type < HOOK_LAST; type++) {
    unsigned int registered = hook_count((enum HookType)type);
    unsigned int calls = hook_calls((enum HookType)type);

    if (!registered && !calls)
      continue;

    send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
               ":Hook %s: %u registered, %u call%s",
               hook_type_name((enum HookType)type), registered, calls,
               calls == 1 ? "" : "s");
  }
}

/** Initialise the module subsystem. */
void module_init(void) {
  manager = (struct ModuleManager *)MyMalloc(sizeof(struct ModuleManager));
  if (!manager) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Failed to allocate struct ModuleManager*");
    exit(1);
  }
  manager->mod_count = 0;
  manager->mod_cb_depth = 0;
  manager->mod_list = 0;
}

/** Unload every module, in reverse order of loading. */
void module_shutdown(void) {
  if (!manager)
    return;

  while (manager->mod_list)
    module_unload(manager->mod_list);
}

/** Release the module manager itself.
 *
 * Separate from module_shutdown() because it ends the module system rather
 * than just emptying it: only the main thread calls this, once, after the
 * event loop has returned and nothing can ask about modules again.  Every
 * accessor treats a NULL manager as "the module system is not up", so a
 * stray later call is refused instead of reaching freed memory, and
 * module_init() would bring it back.
 */
void module_close(void) {
  if (!manager)
    return;

  module_shutdown();

  MyFree(manager);
  manager = 0;
}
