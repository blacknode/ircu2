/* Test fixture: a module whose migrations are malformed. */
#include "module.h"

static int mod_migbad_init(struct ModuleHandle* mod)
{
  (void) mod;
  return 0;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "mod_migbad", "1.0", "test", "module whose migrations are malformed",
  mod_migbad_init, 0, 0
};
