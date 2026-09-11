/* Test fixture: a module that claims the reserved name. */
#include "module.h"

static int mod_core_init(struct ModuleHandle* mod)
{
  (void) mod;
  return 0;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "core", "1.0", "test", "module that claims the reserved name",
  mod_core_init, 0, 0
};
