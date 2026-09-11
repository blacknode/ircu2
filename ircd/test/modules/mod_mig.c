/* Test fixture: a module with a well-formed migration set. */
#include "module.h"

static int mod_mig_init(struct ModuleHandle* mod)
{
  (void) mod;
  return 0;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "mod_mig", "1.0", "test", "module with a well-formed migration set",
  mod_mig_init, 0, 0
};
