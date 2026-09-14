/* Test fixture: registers two capabilities and leaves them for the loader. */
#include "capab.h"
#include "module.h"

/** Positions the server assigned.  They live in the test binary, the same
 * way mod_umode's flags do: a module resolves symbols out of the
 * executable that loaded it, never the other way round.
 */
extern int mod_cap_index_one;
extern int mod_cap_index_two;

static int cap_mod_init(struct ModuleHandle* mod)
{
  if (!module_add_cap(mod, "example.org/one", 0, &mod_cap_index_one))
    return -1;

  /* Deliberately NOT removed in mi_fini: the loader must revert it. */
  if (!module_add_cap(mod, "example.org/two", CAPFL_STICKY,
                      &mod_cap_index_two))
    return -1;

  /* A name the core already owns is refused. */
  if (module_add_cap(mod, "echo-message", 0, 0))
    return -1;

  /* And so is one this module just took. */
  if (module_add_cap(mod, "example.org/one", 0, 0))
    return -1;

  /* And so is a malformed one. */
  if (module_add_cap(mod, "not a name", 0, 0))
    return -1;

  return 0;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "mod_cap", "1.0", "test", "registers two capabilities",
  cap_mod_init, 0, 0
};
