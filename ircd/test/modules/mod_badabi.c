/* Test fixture: declares an ABI the server does not speak. */
#include "module.h"

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI + 1000, "mod_badabi", "1.0", "test", "wrong ABI",
  0, 0, 0
};
