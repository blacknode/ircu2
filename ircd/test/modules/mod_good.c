/* Test fixture: a well-formed module that references a core symbol. */
#include "module.h"

/* Resolved against the test binary's dynamic symbol table at load time. */
extern int module_test_marker;

static int good_init(struct ModuleHandle* mod)
{
  (void) mod;
  module_test_marker += 1;
  return 0;
}

static void good_fini(struct ModuleHandle* mod)
{
  (void) mod;
  module_test_marker += 10;
}

static void good_rehash(struct ModuleHandle* mod)
{
  (void) mod;
  module_test_marker += 100;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "mod_good", "1.0", "test", "well-formed test module",
  good_init, good_fini, good_rehash
};
