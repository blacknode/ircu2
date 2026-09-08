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

struct ModuleHandle;

/** ABI version of the module interface.
 *
 * The server refuses to load a module built against a different value.
 * There is no backwards compatibility: when this changes, modules are
 * recompiled.  A mismatched pointer layout in a shared address space is
 * not a failure worth being lenient about.
 */
#define IRCU_MODULE_ABI 1

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

/*
 * Server-side interface.  Not for use by modules.
 */
extern void module_init(void);
extern void module_shutdown(void);

extern struct ModuleHandle* module_load(const char* path, const char** errstr);
extern int module_unload(struct ModuleHandle* mod);
extern struct ModuleHandle* module_find(const char* name);
extern struct ModuleHandle* module_find_path(const char* path);

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
