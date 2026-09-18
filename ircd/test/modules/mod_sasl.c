/* Test fixture: registers a SASL mechanism and leaves it for the loader. */
#include "module.h"
#include "sasl.h"

#include <string.h>

/** A mechanism whose whole message is the identity.  Enough to prove the
 * registration reaches the exchange; what a real one does with it is the
 * provider's business.
 */
static enum SaslStep mod_sasl_step(struct SaslSession* ses, const char* in,
                                   size_t inlen)
{
  if (inlen + 1 > sizeof(ses->ss_authcid))
    return SASL_STEP_FAIL;

  memcpy(ses->ss_authcid, in, inlen);
  ses->ss_authcid[inlen] = '\0';

  return SASL_STEP_CREDENTIAL;
}

static int sasl_mod_init(struct ModuleHandle* mod)
{
  if (!module_add_sasl_mechanism(mod, "X-TEST", 0, mod_sasl_step))
    return -1;

  /* Deliberately NOT removed in mi_fini: the loader must revert it. */
  if (!module_add_sasl_mechanism(mod, "X-OTHER", SASL_MECH_NEEDS_TLS,
                                 mod_sasl_step))
    return -1;

  /* One the core already owns is refused. */
  if (module_add_sasl_mechanism(mod, "PLAIN", 0, mod_sasl_step))
    return -1;

  /* And so is one this module just took, under either spelling. */
  if (module_add_sasl_mechanism(mod, "x-test", 0, mod_sasl_step))
    return -1;

  /* And so is a malformed name. */
  if (module_add_sasl_mechanism(mod, "not a name", 0, mod_sasl_step))
    return -1;

  /* Removing the core's is refused too. */
  if (module_del_sasl_mechanism(mod, "PLAIN"))
    return -1;

  return 0;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "mod_sasl", "1.0", "test", "registers a SASL mechanism",
  sasl_mod_init, 0, 0
};
