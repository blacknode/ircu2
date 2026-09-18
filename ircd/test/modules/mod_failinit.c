/* Test fixture: refuses to initialise. */
#include "module.h"

static int fail_init(struct ModuleHandle* mod)
{
  (void) mod;
  return -1;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "mod_failinit", "1.0", "test", "always fails init",
  fail_init, 0, 0
};
