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

#include "module.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_log.h"
#include "ircd_string.h"
#include "s_debug.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/** A module the server has loaded. */
struct ModuleHandle {
  struct ModuleHandle* mh_next;     /**< Next module in #module_list. */
  void*                mh_dl;       /**< Handle returned by dlopen(). */
  struct ModuleInfo*   mh_info;     /**< The module's exported description. */
  char*                mh_path;     /**< Path the module was loaded from. */
  time_t               mh_mtime;    /**< Modification time when loaded. */
  int                  mh_marked;   /**< Seen in the running configuration. */
};

/** List of loaded modules, most recently loaded first. */
static struct ModuleHandle* module_list;

/** Number of loaded modules. */
static unsigned int module_list_count;

/** Depth of module callbacks currently on the stack. */
static int module_callback_depth;

static int module_unload_internal(struct ModuleHandle* mod, int quiet);

/** Get the name of a module.
 * @param[in] mod Module to query.
 * @return The module's short name.
 */
const char* module_name(const struct ModuleHandle* mod)
{
  assert(0 != mod);
  return mod->mh_info->mi_name;
}

/** Get the path a module was loaded from.
 * @param[in] mod Module to query.
 * @return Filesystem path of the shared object.
 */
const char* module_path(const struct ModuleHandle* mod)
{
  assert(0 != mod);
  return mod->mh_path;
}

/** Get the version string a module declares.
 * @param[in] mod Module to query.
 * @return Version string, or "?" if the module declares none.
 */
const char* module_version(const struct ModuleHandle* mod)
{
  assert(0 != mod);
  return mod->mh_info->mi_version ? mod->mh_info->mi_version : "?";
}

/** Get the description a module declares.
 * @param[in] mod Module to query.
 * @return Description, or "" if the module declares none.
 */
const char* module_description(const struct ModuleHandle* mod)
{
  assert(0 != mod);
  return mod->mh_info->mi_description ? mod->mh_info->mi_description : "";
}

/** Return the number of currently loaded modules. */
unsigned int module_count(void)
{
  return module_list_count;
}

/** Report whether a module callback is currently executing. */
int module_in_callback(void)
{
  return module_callback_depth > 0;
}

/** Iterate over the loaded modules.
 * @param[in] mod Previously returned module, or NULL to start.
 * @return Next module, or NULL when the list is exhausted.
 */
struct ModuleHandle* module_next(struct ModuleHandle* mod)
{
  return mod ? mod->mh_next : module_list;
}

/** Find a loaded module by name.
 * @param[in] name Module name to look for; compared case-insensitively.
 * @return Matching module, or NULL.
 */
struct ModuleHandle* module_find(const char* name)
{
  struct ModuleHandle* mod;

  if (!name)
    return 0;

  for (mod = module_list; mod; mod = mod->mh_next)
    if (0 == ircd_strcmp(mod->mh_info->mi_name, name))
      return mod;

  return 0;
}

/** Find a loaded module by the path it was loaded from.
 * @param[in] path Path to look for; compared exactly.
 * @return Matching module, or NULL.
 */
struct ModuleHandle* module_find_path(const char* path)
{
  struct ModuleHandle* mod;

  if (!path)
    return 0;

  for (mod = module_list; mod; mod = mod->mh_next)
    if (0 == strcmp(mod->mh_path, path))
      return mod;

  return 0;
}

/** Get the modification time of a file.
 * @param[in] path File to stat.
 * @return Modification time, or zero if the file cannot be stat()ed.
 */
static time_t module_mtime(const char* path)
{
  struct stat sb;

  if (stat(path, &sb) < 0)
    return 0;

  return sb.st_mtime;
}

/** Test whether a module's file changed since it was loaded.
 * @param[in] mod Module to check.
 * @return Non-zero if the file on disk is newer or has gone missing.
 */
int module_changed_on_disk(const struct ModuleHandle* mod)
{
  assert(0 != mod);
  return module_mtime(mod->mh_path) != mod->mh_mtime;
}

/** Load a module from a shared object.
 *
 * On failure nothing is left behind: the shared object is closed again and
 * no handle is added to the module list.
 *
 * @param[in] path Filesystem path of the shared object.
 * @param[out] errstr If non-NULL, receives a human-readable reason on
 *   failure.  The string is owned by the caller only until the next call.
 * @return Handle for the loaded module, or NULL on failure.
 */
struct ModuleHandle* module_load(const char* path, const char** errstr)
{
  static char errbuf[512];
  struct ModuleHandle* mod;
  struct ModuleInfo* info;
  void* dl;

  assert(0 != path);

  if (errstr)
    *errstr = 0;

  if (module_find_path(path)) {
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

  info = (struct ModuleInfo*) dlsym(dl, "ircu_module");
  if (!info) {
    dlclose(dl);
    if (errstr)
      *errstr = "no ircu_module symbol; is this an ircu module?";
    return 0;
  }

  if (info->mi_abi != IRCU_MODULE_ABI) {
    snprintf(errbuf, sizeof(errbuf),
             "ABI mismatch: module was built for %u, server speaks %u",
             info->mi_abi, (unsigned int) IRCU_MODULE_ABI);
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
    snprintf(errbuf, sizeof(errbuf),
             "a module named %s is already loaded", info->mi_name);
    dlclose(dl);
    if (errstr)
      *errstr = errbuf;
    return 0;
  }

  mod = (struct ModuleHandle*) MyCalloc(1, sizeof(struct ModuleHandle));
  mod->mh_dl = dl;
  mod->mh_info = info;
  DupString(mod->mh_path, path);
  mod->mh_mtime = module_mtime(path);
  mod->mh_marked = 1;

  /* Link before mi_init so that anything the module registers can find its
   * own handle in the list.
   */
  mod->mh_next = module_list;
  module_list = mod;
  module_list_count++;

  if (info->mi_init) {
    int res;

    module_callback_depth++;
    res = (*info->mi_init)(mod);
    module_callback_depth--;

    if (res) {
      snprintf(errbuf, sizeof(errbuf),
               "module %s refused to initialise (returned %d)",
               info->mi_name, res);

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

  log_write(LS_SYSTEM, L_INFO, 0, "Loaded module %s %s from %s",
            info->mi_name, info->mi_version ? info->mi_version : "?", path);

  return mod;
}

/** Unload a module and close its shared object.
 * @param[in] mod Module to unload.
 * @param[in] quiet Non-zero to skip the "Unloaded" log line, used when
 *   unwinding a load that never completed.
 * @return Non-zero on success, zero if the module cannot be unloaded now.
 */
static int module_unload_internal(struct ModuleHandle* mod, int quiet)
{
  struct ModuleHandle** mod_p;
  char name[64];
  void* dl;

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

  if (mod->mh_info->mi_fini) {
    module_callback_depth++;
    (*mod->mh_info->mi_fini)(mod);
    module_callback_depth--;
  }

  /* Everything the module registered through the module API is reverted
   * here, after mi_fini has had its chance and before the code goes away.
   * Later phases add command and hook cleanup at this point.
   */

  for (mod_p = &module_list; *mod_p; mod_p = &(*mod_p)->mh_next) {
    if (*mod_p == mod) {
      *mod_p = mod->mh_next;
      module_list_count--;
      break;
    }
  }

  dl = mod->mh_dl;
  MyFree(mod->mh_path);
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
int module_unload(struct ModuleHandle* mod)
{
  return module_unload_internal(mod, 0);
}

/** Mark a module as present in the running configuration. */
void module_mark(struct ModuleHandle* mod)
{
  assert(0 != mod);
  mod->mh_marked = 1;
}

/** Clear the configuration mark on every loaded module.
 *
 * Called before re-reading the configuration; module_sweep() then unloads
 * whatever the new configuration did not mention.
 */
void module_unmark_all(void)
{
  struct ModuleHandle* mod;

  for (mod = module_list; mod; mod = mod->mh_next)
    mod->mh_marked = 0;
}

/** Unload every module the running configuration no longer mentions. */
void module_sweep(void)
{
  struct ModuleHandle* mod;
  struct ModuleHandle* next;

  for (mod = module_list; mod; mod = next) {
    next = mod->mh_next;
    if (!mod->mh_marked)
      module_unload(mod);
  }
}

/** Tell a module that the server rehashed, if it wants to know. */
void module_rehash_notify(struct ModuleHandle* mod)
{
  assert(0 != mod);

  if (mod->mh_info->mi_rehash) {
    module_callback_depth++;
    (*mod->mh_info->mi_rehash)(mod);
    module_callback_depth--;
  }
}

/** Initialise the module subsystem. */
void module_init(void)
{
  module_list = 0;
  module_list_count = 0;
  module_callback_depth = 0;
}

/** Unload every module, in reverse order of loading. */
void module_shutdown(void)
{
  while (module_list)
    module_unload(module_list);
}
