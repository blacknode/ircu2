/* Test fixture: registers two user modes and leaves them for the loader. */
#include "client.h"
#include "module.h"

/** Bits the server assigned.  They live in the test binary, the same way
 * mod_good's marker does: a module resolves symbols out of the executable
 * that loaded it, never the other way round.
 */
extern flag_t mod_umode_flag_one;
extern flag_t mod_umode_flag_two;

static int umode_init(struct ModuleHandle* mod)
{
  if (!module_add_user_mode(mod, 'Y', &mod_umode_flag_one))
    return -1;

  /* Deliberately NOT removed in mi_fini: the loader must revert it. */
  if (!module_add_user_mode(mod, 'Z', &mod_umode_flag_two))
    return -1;

  /* A letter the core already owns is refused. */
  if (module_add_user_mode(mod, 'o', 0))
    return -1;

  /* And so is one this module just took. */
  if (module_add_user_mode(mod, 'Y', 0))
    return -1;

  return 0;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "mod_umode", "1.0", "test", "registers two user modes",
  umode_init, 0, 0
};
