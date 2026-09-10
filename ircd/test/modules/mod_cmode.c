/* Test fixture: registers two channel modes and leaves them for the loader. */
#include "channel.h"
#include "module.h"

/** Bits the letters map to.  They live in the test binary, the same way
 * mod_good's marker does: a module resolves symbols out of the executable
 * that loaded it, never the other way round.
 */
extern chanmode_t mod_cmode_flag_one;
extern chanmode_t mod_cmode_flag_two;

static int cmode_init(struct ModuleHandle* mod)
{
  if (!module_add_chan_mode(mod, 'W', &mod_cmode_flag_one))
    return -1;

  /* Deliberately NOT removed in mi_fini: the loader must revert it. */
  if (!module_add_chan_mode(mod, 'X', &mod_cmode_flag_two))
    return -1;

  /* A letter the core already owns is refused. */
  if (module_add_chan_mode(mod, 'o', 0))
    return -1;

  /* And so is one this module just took. */
  if (module_add_chan_mode(mod, 'W', 0))
    return -1;

  /* And so is something that is not a letter. */
  if (module_add_chan_mode(mod, '1', 0))
    return -1;

  return 0;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "mod_cmode", "1.0", "test", "registers two channel modes",
  cmode_init, 0, 0
};
