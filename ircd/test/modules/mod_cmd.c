/* Test fixture: registers a command and leaves it for the loader to clean up. */
#include "module.h"
#include "msg.h"

static int cmd_handler(struct Client* cptr, struct Client* sptr,
                       int parc, char* parv[])
{
  (void) cptr; (void) sptr; (void) parc; (void) parv;
  return 0;
}

static int cmd_init(struct ModuleHandle* mod)
{
  MessageHandler handlers[LAST_HANDLER_TYPE];
  int i;

  for (i = 0; i < LAST_HANDLER_TYPE; i++)
    handlers[i] = cmd_handler;

  if (!module_add_command(mod, "TESTCMD", "TESTCMD", 1, 0, handlers))
    return -1;

  /* Deliberately NOT removed in mi_fini: the loader must revert it. */
  if (!module_add_command(mod, "TESTCMD2", "TESTCMD2", 1, 0, handlers))
    return -1;

  return 0;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "mod_cmd", "1.0", "test", "registers two commands",
  cmd_init, 0, 0
};
